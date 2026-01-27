import AVFoundation

// MARK: - Audio output (AVAudioEngine)

final class AudioOutput {
    private let handle: OpaquePointer
    private let log: (LogLevel, String) -> Void
    private let engine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?

    // Audio output can be accessed from the emu thread + interruption callbacks
    private let stateLock = NSLock()
    private var engineRunning = false
    private var desiredActive = false
    private var wasInterrupted = false
    private var notificationTokens: [NSObjectProtocol] = []
    private var isMuted = false

    // Core audio format is fixed (S16, stereo, 44.1 kHz)
    private let sampleRate: Double
    private let channels: AVAudioChannelCount = 2

    // Scratch buffer holds interleaved S16 samples from the core
    private let scratchFrames = 4096
    private let scratch: UnsafeMutablePointer<Int16>

    init?(handle: OpaquePointer?, log: @escaping (LogLevel, String) -> Void) {
        guard let handle = handle else {
            return nil
        }
        self.handle = handle
        self.log = log

        var info = ymir_audio_info_t(sample_rate: 0, channels: 0, format: YMIR_AUDIO_FORMAT_S16)
        ymir_get_audio_info(handle, &info)
        sampleRate = Double(info.sample_rate)

        scratch = UnsafeMutablePointer<Int16>.allocate(capacity: scratchFrames * 2)
        scratch.initialize(repeating: 0, count: scratchFrames * 2)

        setupNotifications()
    }

    deinit {
        for token in notificationTokens {
            NotificationCenter.default.removeObserver(token)
        }
        scratch.deinitialize(count: scratchFrames * 2)
        scratch.deallocate()
    }

    // MARK: State

    var isRunning: Bool {
        stateLock.lock()
        let value = engineRunning
        stateLock.unlock()
        return value
    }

    // MARK: Public control

    func start() {
        stateLock.lock()
        desiredActive = true
        let alreadyRunning = engineRunning
        stateLock.unlock()
        if alreadyRunning {
            return
        }
        do {
            try configureSession()
        } catch {
            log(.error, "Audio session error: \(error.localizedDescription)")
        }

        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                   sampleRate: sampleRate,
                                   channels: channels,
                                   interleaved: false)
        guard let format = format else {
            log(.error, "Failed to create audio format")
            return
        }

        // Pull-based render callback: asks the core for audio frames
        let node = AVAudioSourceNode(format: format) { [weak self] _, _, frameCount, bufferList in
            guard let self = self else {
                return noErr
            }
            self.render(frameCount: frameCount, bufferList: bufferList)
            return noErr
        }

        sourceNode = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        engine.prepare()

        do {
            try engine.start()
            stateLock.lock()
            engineRunning = true
            stateLock.unlock()
        } catch {
            log(.error, "Failed to start audio engine: \(error.localizedDescription)")
        }
        applyMuteState()
    }

    func stop() {
        desiredActive = false
        stopEngine()
    }

    func setMuted(_ muted: Bool) {
        stateLock.lock()
        let shouldUpdate = muted != isMuted
        isMuted = muted
        stateLock.unlock()
        if !shouldUpdate {
            return
        }
        applyMuteState()
    }

    // MARK: Engine helpers

    private func stopEngine() {
        stateLock.lock()
        let running = engineRunning
        stateLock.unlock()
        if !running {
            return
        }
        engine.stop()
        if let node = sourceNode {
            engine.detach(node)
            sourceNode = nil
        }
        stateLock.lock()
        engineRunning = false
        stateLock.unlock()
    }

    private func render(frameCount: AVAudioFrameCount, bufferList: UnsafeMutablePointer<AudioBufferList>) {
        let frames = Int(frameCount)
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        if buffers.count < 2 {
            return
        }
        if frames > scratchFrames {
            // Defensive: if the system asks for more than we can stage, output silence
            for buffer in buffers {
                if let data = buffer.mData {
                    memset(data, 0, Int(buffer.mDataByteSize))
                }
            }
            return
        }

        let framesRead = Int(ymir_read_audio_samples(handle, scratch, frames))
        let left = buffers[0].mData!.assumingMemoryBound(to: Float.self)
        let right = buffers[1].mData!.assumingMemoryBound(to: Float.self)
        let scale = Float(1.0 / 32768.0)

        if framesRead > 0 {
            var sourceIndex = 0
            for i in 0..<framesRead {
                left[i] = Float(scratch[sourceIndex]) * scale
                right[i] = Float(scratch[sourceIndex + 1]) * scale
                sourceIndex += 2
            }
        }

        if framesRead < frames {
            // Underrun: zero-fill the remainder
            for i in framesRead..<frames {
                left[i] = 0
                right[i] = 0
            }
        }
    }

    private func configureSession() throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try session.setPreferredSampleRate(sampleRate)
        try session.setActive(true, options: [])
    }

    private func applyMuteState() {
        stateLock.lock()
        let muted = isMuted
        stateLock.unlock()
        let volume: Float = muted ? 0.0 : 1.0
        engine.mainMixerNode.outputVolume = volume
    }

    // MARK: Notifications

    private func setupNotifications() {
        let center = NotificationCenter.default
        let interruptionToken = center.addObserver(forName: AVAudioSession.interruptionNotification,
                                                   object: nil,
                                                   queue: .main) { [weak self] notification in
            self?.handleInterruption(notification)
        }
        let mediaResetToken = center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification,
                                                 object: nil,
                                                 queue: .main) { [weak self] _ in
            self?.handleMediaServicesReset()
        }
        notificationTokens = [interruptionToken, mediaResetToken]
    }

    private func handleInterruption(_ notification: Notification) {
        guard let info = notification.userInfo,
              let typeValue = info[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: typeValue) else {
            return
        }

        switch type {
        case .began:
            // Stop immediately to release the audio device during interruptions
            stateLock.lock()
            wasInterrupted = engineRunning
            stateLock.unlock()
            stopEngine()
        case .ended:
            let optionsValue = info[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
            let options = AVAudioSession.InterruptionOptions(rawValue: optionsValue)
            stateLock.lock()
            let shouldResume = desiredActive && (options.contains(.shouldResume) || wasInterrupted)
            wasInterrupted = false
            stateLock.unlock()
            if shouldResume {
                start()
            }
        @unknown default:
            break
        }
    }

    private func handleMediaServicesReset() {
        stateLock.lock()
        let shouldRestart = desiredActive
        stateLock.unlock()
        if shouldRestart {
            start()
        }
    }
}
