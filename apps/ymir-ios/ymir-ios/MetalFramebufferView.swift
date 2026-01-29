import MetalKit
import SwiftUI

// MARK: - Metal framebuffer view

struct MetalFramebufferView: UIViewRepresentable {
    // Observe emulator state so the view resumes correctly after tab switches
    @ObservedObject var emulator: EmulatorController
    var options: FrameProcessingOptions = .default

    func makeUIView(context: Context) -> MTKView {
        // init view ( +pixfmt, clearCol)
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = options.drawablePixelFormat
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.autoResizeDrawable = true
        // init view depending on emu state
        applyEmulatorState(view: view)

        // cofigure the view to be updated when emulator changes
        if let renderer = MetalFramebufferRenderer(view: view, emulator: emulator, options: options) {
            context.coordinator.renderer = renderer
            view.delegate = renderer
        }

        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        if uiView.colorPixelFormat != options.drawablePixelFormat {
            uiView.colorPixelFormat = options.drawablePixelFormat
        }

        if uiView.delegate == nil, let renderer = MetalFramebufferRenderer(view: uiView, emulator: emulator,
                                                                           options: options) {
            context.coordinator.renderer = renderer
            uiView.delegate = renderer
        }
        if let renderer = context.coordinator.renderer {
            renderer.updateOptions(options)
        }
        applyEmulatorState(view: uiView)
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    final class Coordinator {
        var renderer: MetalFramebufferRenderer?
    }

    // change metal view depending on emulator state
    // Keep MTKView paused when emulation is idle to avoid idle CPU churn.
    private func applyEmulatorState(view: MTKView) {
        if emulator.isRunning {
            view.preferredFramesPerSecond = 60
            view.isPaused = false
            view.enableSetNeedsDisplay = false
            // Nudge the view to render a frame after state changes or tab switches
            view.setNeedsDisplay()
        } else {
            view.preferredFramesPerSecond = 1   // min for "paused state"
            view.isPaused = true
            view.enableSetNeedsDisplay = true
            // Draw once to clear any stale content when stopping
            view.setNeedsDisplay()
        }
    }
}

extension MetalFramebufferView {
    struct FrameProcessingOptions: Equatable {
        var upscaler: Upscaler
        var shader: ShaderSelection
        var drawablePixelFormat: MTLPixelFormat

        static let `default` = FrameProcessingOptions(upscaler: .none,
                                                      shader: .default,
                                                      drawablePixelFormat: .rgba8Unorm)
    }

    enum Upscaler: Equatable {
        case none
        case metalFXSpatial
    }

    struct ShaderSelection: Hashable {
        var fragmentFunctionName: String
        var label: String

        static let `default` = ShaderSelection(fragmentFunctionName: "ymir_fragment", label: "Default")
    }
}

final class MetalFramebufferRenderer: NSObject, MTKViewDelegate {
    private struct PipelineKey: Hashable {
        let fragmentFunctionName: String
        let pixelFormat: MTLPixelFormat
    }

    private struct PostProcessPass {
        let label: String
        let pipelineState: MTLRenderPipelineState
        let inputTexture: MTLTexture
        let renderPassDescriptor: MTLRenderPassDescriptor
        let drawable: MTLDrawable?
    }

    private struct PostProcessGraph {
        let passes: [PostProcessPass]
    }

    // MARK: - Thread Safety

    /// A minimal wrapper around os_unfair_lock for safe usage in Swift.
    /// Lock ordering: If multiple locks are needed, always acquire cacheLock before optionsLock.
    private final class UnfairLock {
        private var _lock = os_unfair_lock()

        /// Acquires the lock, executes the closure, then releases.
        /// Keep critical sections minimal - do not hold while encoding Metal commands.
        func withLock<T>(_ body: () throws -> T) rethrows -> T {
            os_unfair_lock_lock(&_lock)
            defer { os_unfair_lock_unlock(&_lock) }
            return try body()
        }
    }

    // MARK: - Properties

    private let emulator: EmulatorController
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let library: MTLLibrary
    private let vertexFunction: MTLFunction

    // Thread-safe state: protected by cacheLock
    private let cacheLock = UnfairLock()
    private var pipelineCache: [PipelineKey: MTLRenderPipelineState] = [:]
    // LRU eviction support: ordered list of keys (oldest at index 0, newest at end)
    private var pipelineCacheLRU: [PipelineKey] = []
    private let maxCacheSize = 8
    
    #if DEBUG
    // Telemetry (compiled out in Release builds)
    private var cacheHits: UInt64 = 0
    private var cacheMisses: UInt64 = 0
    private var cacheEvictions: UInt64 = 0
    #endif

    // Thread-safe state: protected by optionsLock
    private let optionsLock = UnfairLock()
    private var options: MetalFramebufferView.FrameProcessingOptions

    // Single-thread state: only accessed from Metal render thread
    private var framebufferTexture: MTLTexture?
    private var currentFramebufferPixelFormat: MTLPixelFormat = .invalid
    private var stagingBuffer: MTLBuffer?
    private var lastFrameId: UInt64 = 0
    private var currentWidth: Int = 0
    private var currentHeight: Int = 0

    private let maxFramebufferBytes = 704 * 512 * 4

    init?(view: MTKView, emulator: EmulatorController, options: MetalFramebufferView.FrameProcessingOptions) {
        guard let device = view.device else {
            return nil
        }
        guard let commandQueue = device.makeCommandQueue() else {
            return nil
        }
        guard let library = device.makeDefaultLibrary(),
              let vertex = library.makeFunction(name: "ymir_vertex") else {
            return nil
        }

        self.emulator = emulator
        self.device = device
        self.commandQueue = commandQueue
        self.library = library
        self.vertexFunction = vertex
        self.stagingBuffer = device.makeBuffer(length: maxFramebufferBytes, options: .storageModeShared)
        self.options = options
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // No-op: MTKView drives drawable sizing for the post-process output.
    }

    func draw(in view: MTKView) {
        guard let stagingBuffer = stagingBuffer else {
            return
        }

        var info = ymir_framebuffer_info_t()
        var frameId: UInt64 = 0
        let ok = emulator.copyFramebuffer(into: stagingBuffer.contents(),
                                           byteCount: stagingBuffer.length,
                                           info: &info,
                                           frameId: &frameId)
        if !ok || frameId == lastFrameId {
            return
        }
        lastFrameId = frameId

        let width = Int(info.width)
        let height = Int(info.height)
        if width <= 0 || height <= 0 {
            return
        }

        let inputPixelFormat = framebufferPixelFormat(for: info.format)
        if framebufferTexture == nil || width != currentWidth || height != currentHeight ||
            currentFramebufferPixelFormat != inputPixelFormat {
            currentWidth = width
            currentHeight = height
            currentFramebufferPixelFormat = inputPixelFormat
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: inputPixelFormat,
                                                                      width: width,
                                                                      height: height,
                                                                      mipmapped: false)
            descriptor.usage = [.shaderRead]
            framebufferTexture = device.makeTexture(descriptor: descriptor)
        }

        guard let framebufferTexture = framebufferTexture else {
            return
        }

        let bytesPerRow = Int(info.stride_bytes)
        let region = MTLRegionMake2D(0, 0, width, height)
        framebufferTexture.replace(region: region, mipmapLevel: 0, withBytes: stagingBuffer.contents(),
                                   bytesPerRow: bytesPerRow)

        guard let graph = buildPostProcessGraph(inputTexture: framebufferTexture, view: view) else {
            return
        }

        guard let commandBuffer = commandQueue.makeCommandBuffer() else {
            return
        }

        var drawableToPresent: MTLDrawable?
        for pass in graph.passes {
            guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass.renderPassDescriptor) else {
                return
            }
            encoder.setRenderPipelineState(pass.pipelineState)
            encoder.setFragmentTexture(pass.inputTexture, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
            if let drawable = pass.drawable {
                drawableToPresent = drawable
            }
        }

        if let drawable = drawableToPresent {
            commandBuffer.present(drawable)
        }
        commandBuffer.commit()
        emulator.noteFramePresented(frameId: frameId)
    }

    /// Updates processing options from the main thread.
    /// Thread-safe: acquires optionsLock briefly.
    func updateOptions(_ options: MetalFramebufferView.FrameProcessingOptions) {
        optionsLock.withLock {
            if self.options != options {
                self.options = options
            }
        }
    }

    private func framebufferPixelFormat(for format: ymir_framebuffer_format_t) -> MTLPixelFormat {
        switch format {
        case YMIR_FRAMEBUFFER_FORMAT_XBGR8888:
            // Little-endian XBGR8888 means bytes are RGBA in memory.
            return .rgba8Unorm
        default:
            return .rgba8Unorm
        }
    }

    /// Returns a cached pipeline state or creates one on demand.
    /// Thread-safe: cache access is locked; PSO compilation is outside the lock.
    /// Lock ordering: cacheLock is acquired independently (no nested locks here).
    private func pipelineState(for shader: MetalFramebufferView.ShaderSelection,
                               pixelFormat: MTLPixelFormat) -> MTLRenderPipelineState? {
        let key = PipelineKey(fragmentFunctionName: shader.fragmentFunctionName, pixelFormat: pixelFormat)

        // Fast path: check cache under lock (minimal critical section)
        if let cached = cacheLock.withLock({ () -> MTLRenderPipelineState? in
            #if DEBUG
            if let pso = pipelineCache[key] {
                cacheHits += 1
                return pso
            }
            #else
            if let pso = pipelineCache[key] {
                return pso
            }
            #endif
            return nil
        }) {
            return cached
        }

        // Slow path: PSO compilation happens OUTSIDE the lock (can take 10-50ms)
        #if DEBUG
        cacheLock.withLock { cacheMisses += 1 }
        #endif

        guard let fragmentFunction = library.makeFunction(name: shader.fragmentFunctionName) else {
            if shader != .default {
                return pipelineState(for: .default, pixelFormat: pixelFormat)
            }
            return nil
        }

        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = vertexFunction
        pipelineDescriptor.fragmentFunction = fragmentFunction
        pipelineDescriptor.colorAttachments[0].pixelFormat = pixelFormat

        guard let newPipelineState = try? device.makeRenderPipelineState(descriptor: pipelineDescriptor) else {
            if shader != .default {
                return pipelineState(for: .default, pixelFormat: pixelFormat)
            }
            return nil
        }

        // Store in cache under lock; handle potential race (another thread may have inserted)
        cacheLock.withLock {
            // Double-check: if another thread already inserted, use theirs (prevent duplicate PSOs)
            if pipelineCache[key] == nil {
                // LRU eviction: remove oldest entry if cache is full
                if pipelineCacheLRU.count >= maxCacheSize {
                    let evictedKey = pipelineCacheLRU.removeFirst()
                    pipelineCache.removeValue(forKey: evictedKey)
                    #if DEBUG
                    cacheEvictions += 1
                    print("[MetalRenderer] PSO eviction: \(evictedKey.fragmentFunctionName)@\(evictedKey.pixelFormat.rawValue)")
                    #endif
                }
                pipelineCache[key] = newPipelineState
                pipelineCacheLRU.append(key)
                #if DEBUG
                print("[MetalRenderer] PSO compiled: \(shader.fragmentFunctionName)@\(pixelFormat.rawValue) (cache: \(pipelineCacheLRU.count)/\(maxCacheSize))")
                #endif
            }
        }

        return newPipelineState
    }


    /// Builds the post-processing graph for this frame.
    /// Thread-safe: reads options once under lock, then uses the snapshot.
    private func buildPostProcessGraph(inputTexture: MTLTexture, view: MTKView) -> PostProcessGraph? {
        guard let renderPass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable else {
            return nil
        }

        // Snapshot options under lock - minimal critical section
        let currentOptions = optionsLock.withLock { options }

        var passes: [PostProcessPass] = []
        var workingTexture = inputTexture

        if currentOptions.upscaler != .none {
            // Placeholder for a future upscaler stage (MetalFX or shader-based).
            workingTexture = inputTexture
        }

        guard let pipelineState = pipelineState(for: currentOptions.shader, pixelFormat: view.colorPixelFormat) else {
            return nil
        }

        passes.append(PostProcessPass(label: "present",
                                      pipelineState: pipelineState,
                                      inputTexture: workingTexture,
                                      renderPassDescriptor: renderPass,
                                      drawable: drawable))

        return PostProcessGraph(passes: passes)
    }
}
