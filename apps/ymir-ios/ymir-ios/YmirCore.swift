import Foundation
import QuartzCore
import SwiftUI

// MARK: - Core log callback

// devlog callback via FFI; forwards to the UI log store
private let ymirLogCallback: ymir_log_callback_t = { userData, level, message in
    guard let message = message else {
        return
    }
    guard let userData = userData else {
        return
    }
    let text = String(cString: message)
    let controller = Unmanaged<EmulatorController>.fromOpaque(userData).takeUnretainedValue()
    controller.logStore.append(level: LogLevel(cLevel: level), message: text)
}

// MARK: - Metrics

struct MetricsSnapshot {
    var fps: Double = 0.0
    var frameAgeSeconds: Double? = nil
    var lastFrameId: UInt64 = 0

    static let empty = MetricsSnapshot()
}

// Lightweight, thread-safe metrics tracker for frame presentation.
final class MetricsTracker {
    private let lock = NSLock()
    private var frameCount: UInt64 = 0
    private var lastFrameTime: TimeInterval = 0
    private var lastFrameId: UInt64 = 0
    private var lastSampleTime: TimeInterval = CACurrentMediaTime()
    private var lastSampleFrameCount: UInt64 = 0

    func noteFramePresented(frameId: UInt64) {
        let now = CACurrentMediaTime()
        lock.lock()
        frameCount += 1
        lastFrameTime = now
        lastFrameId = frameId
        lock.unlock()
    }

    func reset() {
        lock.lock()
        frameCount = 0
        lastFrameTime = 0
        lastFrameId = 0
        lastSampleTime = CACurrentMediaTime()
        lastSampleFrameCount = 0
        lock.unlock()
    }

    func sample(now: TimeInterval) -> MetricsSnapshot {
        lock.lock()
        let totalFrames = frameCount
        let frameTime = lastFrameTime
        let frameId = lastFrameId
        let sampleTime = lastSampleTime
        let sampleFrames = lastSampleFrameCount
        lastSampleTime = now
        lastSampleFrameCount = totalFrames
        lock.unlock()

        let deltaFrames = totalFrames &- sampleFrames
        let deltaTime = max(now - sampleTime, 0.001)
        let fps = Double(deltaFrames) / deltaTime
        let age = frameTime > 0 ? now - frameTime : nil
        return MetricsSnapshot(fps: fps, frameAgeSeconds: age, lastFrameId: frameId)
    }
}

// MARK: - Emulator controller

// Owns a single emulation instance and its dedicated emulation thread
final class EmulatorController: ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var statusText = "Idle"
    @Published private(set) var iplName = "No IPL loaded"
    @Published private(set) var audioEnabled = true
    @Published private(set) var audioBufferFill: Double = 0.0
    @Published private(set) var audioQueuedFrames = 0
    @Published private(set) var audioCapacityFrames = 0
    @Published private(set) var metrics = MetricsSnapshot.empty

    let logStore = LogStore()

    // MARK: Emulation state
    private var handle: OpaquePointer?
    private var audioOutput: AudioOutput?
    private let stateLock = NSLock()
    private var runningInternal = false
    private var shouldShutdown = false
    private var metricsVisible = true
    private var iplPath: String?

    // MARK: Threading primitives
    private var emuThread: Thread?
    private let commandCondition = NSCondition()
    private var commandQueue: [EmuCommand] = []
    private let shutdownSemaphore = DispatchSemaphore(value: 0)

    // Audio buffer polling uses a background timer to avoid SwiftUI-driven timers
    private var audioRefreshTimer: DispatchSourceTimer?
    private var metricsTimer: DispatchSourceTimer?
    private let metricsTracker = MetricsTracker()

    // Commands are the only way to mutate core state; processed on the emu thread
    private enum EmuCommand {
        case start
        case stop
        case reset(hard: Bool)
        case loadIPL(path: String)
        case shutdown
    }

    // MARK: Lifecycle

    init() {
        var config = ymir_config_t(struct_size: UInt32(MemoryLayout<ymir_config_t>.size), flags: 0)
        handle = ymir_create(&config)
        if let handle = handle {
            let userData = Unmanaged.passUnretained(self).toOpaque()
            ymir_set_log_callback(handle, ymirLogCallback, userData)
            audioOutput = AudioOutput(handle: handle) { [weak self] level, message in
                self?.logStore.append(level: level, message: message)
            }
            audioOutput?.setMuted(!audioEnabled)
            if let version = ymir_get_version_string() {
                logStore.append(level: .info, message: "Ymir core ready (\(String(cString: version)))")
            }
            startEmuThread()
            startAudioRefreshTimer()
        } else {
            logStore.append(level: .error, message: "Failed to create Ymir core instance")
        }
    }

    deinit {
        audioRefreshTimer?.cancel()
        metricsTimer?.cancel()
        if emuThread != nil {
            enqueueCommand(.shutdown)
            shutdownSemaphore.wait()
        }
        audioOutput?.stop()
        audioOutput = nil
        if let handle = handle {
            ymir_set_log_callback(handle, nil, nil)
            ymir_destroy(handle)
        }
    }

    // MARK: Public API

    func loadIPL(from url: URL) {
        enqueueCommand(.stop)

        let fileManager = FileManager.default
        let docsURL = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first
        guard let docsURL = docsURL else {
            logStore.append(level: .error, message: "Unable to access Documents directory")
            return
        }

        let destination = docsURL.appendingPathComponent(url.lastPathComponent)
        let hasAccess = url.startAccessingSecurityScopedResource()
        defer {
            if hasAccess {
                url.stopAccessingSecurityScopedResource()
            }
        }

        do {
            if fileManager.fileExists(atPath: destination.path) {
                try fileManager.removeItem(at: destination)
            }
            try fileManager.copyItem(at: url, to: destination)
        } catch {
            logStore.append(level: .error, message: "Failed to copy IPL ROM: \(error.localizedDescription)")
            return
        }

        DispatchQueue.main.async { [weak self] in
            self?.iplName = destination.lastPathComponent
            self?.statusText = "IPL ready"
        }

        enqueueCommand(.loadIPL(path: destination.path))
    }

    func start() {
        enqueueCommand(.start)
    }

    func stop() {
        enqueueCommand(.stop)
    }

    func setAudioEnabled(_ enabled: Bool) {
        if enabled == audioEnabled {
            return
        }
        audioEnabled = enabled
        audioOutput?.setMuted(!enabled)
    }

    // Metrics UI can be hidden when the Home tab isn't visible.
    // This keeps the background timer off when metrics are not displayed.
    func setMetricsVisible(_ visible: Bool) {
        stateLock.lock()
        if metricsVisible == visible {
            stateLock.unlock()
            return
        }
        let shouldReset = visible
        metricsVisible = visible
        stateLock.unlock()
        if shouldReset {
            metricsTracker.reset()
            DispatchQueue.main.async { [weak self] in
                self?.metrics = MetricsSnapshot.empty
            }
        }
        updateMetricsTimerState()
    }

    // Called by the renderer when a frame is presented.
    func noteFramePresented(frameId: UInt64) {
        metricsTracker.noteFramePresented(frameId: frameId)
    }

    // Updates audio buffer state; safe to call from a background thread
    func refreshAudioBufferState() {
        guard let handle = handle else {
            return
        }
        var state = ymir_audio_buffer_state_t(queued_frames: 0, capacity_frames: 0)
        ymir_get_audio_buffer_state(handle, &state)
        let capacity = Int(state.capacity_frames)
        let queued = Int(state.queued_frames)
        let fill = capacity > 0 ? Double(queued) / Double(capacity) : 0.0
        if Thread.isMainThread {
            audioCapacityFrames = capacity
            audioQueuedFrames = queued
            audioBufferFill = fill
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.audioCapacityFrames = capacity
                self?.audioQueuedFrames = queued
                self?.audioBufferFill = fill
            }
        }
    }

    // If audio buffers drift up, insert tiny sleeps to keep latency bounded
    private func applyAudioBackpressure(_ handle: OpaquePointer) {
        guard let output = audioOutput, output.isRunning else {
            return
        }

        var state = ymir_audio_buffer_state_t(queued_frames: 0, capacity_frames: 0)
        ymir_get_audio_buffer_state(handle, &state)

        let capacity = Int(state.capacity_frames)
        if capacity <= 0 {
            return
        }

        let maxFrames = Int(Double(capacity) * 0.35)
        let targetFrames = Int(Double(capacity) * 0.25)
        var queued = Int(state.queued_frames)
        var loops = 0

        while queued > maxFrames && loops < 8 {
            Thread.sleep(forTimeInterval: 0.001)
            ymir_get_audio_buffer_state(handle, &state)
            queued = Int(state.queued_frames)
            if queued <= targetFrames {
                break
            }
            loops += 1
        }
    }

    // MARK: Emu thread

    private func startEmuThread() {
        guard emuThread == nil else {
            return
        }
        let thread = Thread { [weak self] in
            self?.emuThreadMain()
        }
        thread.name = "ymir.emu.thread"
        thread.start()
        emuThread = thread
    }

    // Audio polling timer runs off the main thread to avoid SwiftUI refresh churn
    private func startAudioRefreshTimer() {
        if audioRefreshTimer != nil {
            return
        }
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + 0.25, repeating: 0.25)
        timer.setEventHandler { [weak self] in
            guard let self = self else {
                return
            }
            guard let output = self.audioOutput, output.isRunning else {
                return
            }
            self.refreshAudioBufferState()
        }
        timer.resume()
        audioRefreshTimer = timer
    }

    // Metrics sampling runs only while emulation is active to avoid idle UI churn.
    private func startMetricsTimer() {
        if metricsTimer != nil {
            return
        }
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + 0.5, repeating: 0.5)
        timer.setEventHandler { [weak self] in
            guard let self = self else {
                return
            }
            if !self.isRunningInternal() {
                return
            }
            let snapshot = self.metricsTracker.sample(now: CACurrentMediaTime())
            DispatchQueue.main.async { [weak self] in
                self?.metrics = snapshot
            }
        }
        timer.resume()
        metricsTimer = timer
    }

    private func stopMetricsTimer() {
        metricsTimer?.cancel()
        metricsTimer = nil
    }

    private func updateMetricsTimerState() {
        if shouldRunMetricsTimer() {
            startMetricsTimer()
        } else {
            stopMetricsTimer()
        }
    }

    private func emuThreadMain() {
        guard let handle = handle else {
            shutdownSemaphore.signal()
            return
        }
        // Wall-clock pacing for v1.4; audio-driven pacing comes in v1.5
        let frameDuration = 1.0 / 60.0
        var nextFrameTime = CACurrentMediaTime()

        while true {
            let commands = dequeueCommands()
            for command in commands {
                switch command {
                case .start:
                    if isRunningInternal() {
                        break
                    }
                    guard iplPath != nil else {
                        logStore.append(level: .warn, message: "Load an IPL ROM before starting")
                        break
                    }
                    let resetResult = ymir_reset(handle, true)
                    if resetResult != YMIR_RESULT_OK {
                        let errorMessage = lastErrorMessage() ?? "Failed to reset before start"
                        logStore.append(level: .error, message: errorMessage)
                        break
                    }
                    logStore.append(level: .info, message: "Emulation reset")
                    audioOutput?.start()
                    audioOutput?.setMuted(!audioEnabled)
                    setRunningInternal(true)
                    metricsTracker.reset()
                    updateMetricsTimerState()
                    DispatchQueue.main.async { [weak self] in
                        self?.metrics = MetricsSnapshot.empty
                    }
                    nextFrameTime = CACurrentMediaTime()
                    DispatchQueue.main.async { [weak self] in
                        self?.isRunning = true
                        self?.statusText = "Running"
                    }
                case .stop:
                    if !isRunningInternal() {
                        break
                    }
                    setRunningInternal(false)
                    audioOutput?.stop()
                    metricsTracker.reset()
                    updateMetricsTimerState()
                    DispatchQueue.main.async { [weak self] in
                        self?.metrics = MetricsSnapshot.empty
                    }
                    DispatchQueue.main.async { [weak self] in
                        self?.isRunning = false
                        self?.statusText = "Stopped"
                    }
                case .reset(let hard):
                    _ = ymir_reset(handle, hard)
                case .loadIPL(let path):
                    let result = ymir_set_ipl_path(handle, path)
                    if result != YMIR_RESULT_OK {
                        let errorMessage = lastErrorMessage() ?? "Failed to load IPL ROM"
                        logStore.append(level: .error, message: errorMessage)
                        break
                    }
                    _ = ymir_reset(handle, true)
                    iplPath = path
                    logStore.append(level: .info, message: "IPL ROM loaded and reset")
                case .shutdown:
                    setShouldShutdown()
                    updateMetricsTimerState()
                }
            }

            if isShutdownRequested() {
                break
            }

            if isRunningInternal() {
                ymir_run_frame(handle)
                applyAudioBackpressure(handle)
                nextFrameTime += frameDuration
                let now = CACurrentMediaTime()
                let sleepTime = nextFrameTime - now
                if sleepTime > 0 {
                    Thread.sleep(forTimeInterval: sleepTime)
                } else if sleepTime < -frameDuration {
                    nextFrameTime = now
                }
            } else {
                commandCondition.lock()
                if commandQueue.isEmpty && !isRunningInternal() && !isShutdownRequested() {
                    commandCondition.wait()
                }
                commandCondition.unlock()
            }
        }

        audioOutput?.stop()
        shutdownSemaphore.signal()
    }

    // MARK: Command queue

    private func enqueueCommand(_ command: EmuCommand) {
        commandCondition.lock()
        commandQueue.append(command)
        commandCondition.signal()
        commandCondition.unlock()
    }

    private func dequeueCommands() -> [EmuCommand] {
        commandCondition.lock()
        let commands = commandQueue
        commandQueue.removeAll()
        commandCondition.unlock()
        return commands
    }

    private func setShouldShutdown() {
        stateLock.lock()
        shouldShutdown = true
        runningInternal = false
        stateLock.unlock()
    }

    private func isShutdownRequested() -> Bool {
        stateLock.lock()
        let value = shouldShutdown
        stateLock.unlock()
        return value
    }

    // MARK: Core accessors

    private func lastErrorMessage() -> String? {
        guard let handle = handle else {
            return nil
        }
        guard let message = ymir_get_last_error(handle) else {
            return nil
        }
        return String(cString: message)
    }

    func copyFramebuffer(into buffer: UnsafeMutableRawPointer, byteCount: Int,
                         info: inout ymir_framebuffer_info_t, frameId: inout UInt64) -> Bool {
        guard let handle = handle else {
            return false
        }
        let result = ymir_copy_framebuffer(handle, buffer, byteCount, &info, &frameId)
        return result == YMIR_RESULT_OK
    }

    func setControlPadButtons(_ buttons: UInt16, port: UInt32 = 1) {
        guard let handle = handle else {
            return
        }
        _ = ymir_set_control_pad_buttons(handle, port, buttons)
    }

    // MARK: Thread-safe flags

    private func setRunningInternal(_ value: Bool) {
        stateLock.lock()
        runningInternal = value
        stateLock.unlock()
    }

    private func isRunningInternal() -> Bool {
        stateLock.lock()
        let value = runningInternal
        stateLock.unlock()
        return value
    }

    private func shouldRunMetricsTimer() -> Bool {
        stateLock.lock()
        let shouldRun = runningInternal && metricsVisible
        stateLock.unlock()
        return shouldRun
    }
}
