import Foundation
import QuartzCore
import SwiftUI

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

final class EmulatorController: ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var statusText = "Idle"
    @Published private(set) var iplName = "No IPL loaded"
    @Published private(set) var audioEnabled = true
    @Published private(set) var audioBufferFill: Double = 0.0
    @Published private(set) var audioQueuedFrames = 0
    @Published private(set) var audioCapacityFrames = 0

    let logStore = LogStore()

    private var handle: OpaquePointer?
    private var audioOutput: AudioOutput?
    private let runQueue = DispatchQueue(label: "ymir.emu.run", qos: .userInitiated)
    private let runStateLock = NSLock()
    private var runningInternal = false
    private var iplPath: String?

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
        } else {
            logStore.append(level: .error, message: "Failed to create Ymir core instance")
        }
    }

    deinit {
        stop()
        runQueue.sync {
        }
        audioOutput?.stop()
        audioOutput = nil
        if let handle = handle {
            ymir_set_log_callback(handle, nil, nil)
            ymir_destroy(handle)
        }
    }

    func loadIPL(from url: URL) {
        stop()

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

        runQueue.async { [weak self] in
            guard let self = self, let handle = self.handle else {
                return
            }
            let result = ymir_set_ipl_path(handle, destination.path)
            if result != YMIR_RESULT_OK {
                let errorMessage = self.lastErrorMessage() ?? "Failed to load IPL ROM"
                self.logStore.append(level: .error, message: errorMessage)
                return
            }
            _ = ymir_reset(handle, true)
            self.iplPath = destination.path
            self.logStore.append(level: .info, message: "IPL ROM loaded and reset")
        }
    }

    func start() {
        runQueue.async { [weak self] in
            guard let self = self, let handle = self.handle else {
                return
            }
            if self.isRunningInternal() {
                return
            }
            guard self.iplPath != nil else {
                self.logStore.append(level: .warn, message: "Load an IPL ROM before starting")
                return
            }
            let resetResult = ymir_reset(handle, true)
            if resetResult != YMIR_RESULT_OK {
                let errorMessage = self.lastErrorMessage() ?? "Failed to reset before start"
                self.logStore.append(level: .error, message: errorMessage)
                return
            }
            self.logStore.append(level: .info, message: "Emulation reset")
            self.audioOutput?.start()
            self.audioOutput?.setMuted(!self.audioEnabled)
            self.setRunningInternal(true)
            DispatchQueue.main.async { [weak self] in
                self?.isRunning = true
                self?.statusText = "Running"
            }
            let frameDuration = 1.0 / 60.0
            var nextFrameTime = CACurrentMediaTime()
            while self.isRunningInternal() {
                ymir_run_frame(handle)
                self.applyAudioBackpressure(handle)
                nextFrameTime += frameDuration
                let now = CACurrentMediaTime()
                let sleepTime = nextFrameTime - now
                if sleepTime > 0 {
                    Thread.sleep(forTimeInterval: sleepTime)
                } else if sleepTime < -frameDuration {
                    nextFrameTime = now
                }
            }
            DispatchQueue.main.async { [weak self] in
                if self?.isRunning == true {
                    self?.isRunning = false
                    self?.statusText = "Stopped"
                }
            }
            self.audioOutput?.stop()
        }
    }

    func stop() {
        if !isRunningInternal() {
            return
        }
        setRunningInternal(false)
        audioOutput?.stop()
        DispatchQueue.main.async { [weak self] in
            self?.isRunning = false
            self?.statusText = "Stopped"
        }
    }

    func setAudioEnabled(_ enabled: Bool) {
        if enabled == audioEnabled {
            return
        }
        audioEnabled = enabled
        audioOutput?.setMuted(!enabled)
    }

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

    private func setRunningInternal(_ value: Bool) {
        runStateLock.lock()
        runningInternal = value
        runStateLock.unlock()
    }

    private func isRunningInternal() -> Bool {
        runStateLock.lock()
        let value = runningInternal
        runStateLock.unlock()
        return value
    }
}
