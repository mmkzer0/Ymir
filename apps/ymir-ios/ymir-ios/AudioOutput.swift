import AVFoundation

final class AudioOutput {
    private let handle: OpaquePointer
    private let log: (LogLevel, String) -> Void
    private let engine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?
    private var isRunning = false

    private let sampleRate: Double
    private let channels: AVAudioChannelCount = 2
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
    }

    deinit {
        scratch.deinitialize(count: scratchFrames * 2)
        scratch.deallocate()
    }

    func start() {
        if isRunning {
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
            isRunning = true
        } catch {
            log(.error, "Failed to start audio engine: \(error.localizedDescription)")
        }
    }

    func stop() {
        if !isRunning {
            return
        }
        engine.stop()
        if let node = sourceNode {
            engine.detach(node)
            sourceNode = nil
        }
        isRunning = false
    }

    private func render(frameCount: AVAudioFrameCount, bufferList: UnsafeMutablePointer<AudioBufferList>) {
        let frames = Int(frameCount)
        let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
        if buffers.count < 2 {
            return
        }
        if frames > scratchFrames {
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
}
