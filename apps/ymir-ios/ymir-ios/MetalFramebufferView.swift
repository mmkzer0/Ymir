import MetalKit
import SwiftUI

// MARK: - Metal framebuffer view

struct MetalFramebufferView: UIViewRepresentable {
    // Observe emulator state so the view resumes correctly after tab switches
    @ObservedObject var emulator: EmulatorController

    func makeUIView(context: Context) -> MTKView {
        // init view ( +pixfmt, clearCol)
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .rgba8Unorm
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        // init view depending on emu state
        applyEmulatorState(view: view)

        // cofigure the view to be updated when emulator changes
        if let renderer = MetalFramebufferRenderer(view: view, emulator: emulator) {
            context.coordinator.renderer = renderer
            view.delegate = renderer
        }

        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        if uiView.delegate == nil, let renderer = MetalFramebufferRenderer(view: uiView, emulator: emulator) {
            context.coordinator.renderer = renderer
            uiView.delegate = renderer
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

final class MetalFramebufferRenderer: NSObject, MTKViewDelegate {
    private let emulator: EmulatorController
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState
    private var texture: MTLTexture?
    private var stagingBuffer: MTLBuffer?
    private var lastFrameId: UInt64 = 0
    private var currentWidth: Int = 0
    private var currentHeight: Int = 0

    private let maxFramebufferBytes = 704 * 512 * 4

    init?(view: MTKView, emulator: EmulatorController) {
        guard let device = view.device else {
            return nil
        }
        guard let commandQueue = device.makeCommandQueue() else {
            return nil
        }
        guard let library = device.makeDefaultLibrary(),
              let vertex = library.makeFunction(name: "ymir_vertex"),
              let fragment = library.makeFunction(name: "ymir_fragment") else {
            return nil
        }

        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = vertex
        pipelineDescriptor.fragmentFunction = fragment
        pipelineDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat

        guard let pipelineState = try? device.makeRenderPipelineState(descriptor: pipelineDescriptor) else {
            return nil
        }

        self.emulator = emulator
        self.device = device
        self.commandQueue = commandQueue
        self.pipelineState = pipelineState
        self.stagingBuffer = device.makeBuffer(length: maxFramebufferBytes, options: .storageModeShared)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // No-op: drawable size is driven by the framebuffer resolution.
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

        if texture == nil || width != currentWidth || height != currentHeight {
            currentWidth = width
            currentHeight = height
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: view.colorPixelFormat,
                                                                      width: width,
                                                                      height: height,
                                                                      mipmapped: false)
            descriptor.usage = [.shaderRead]
            texture = device.makeTexture(descriptor: descriptor)
            view.drawableSize = CGSize(width: width, height: height)
        }

        guard let texture = texture else {
            return
        }

        let bytesPerRow = Int(info.stride_bytes)
        let region = MTLRegionMake2D(0, 0, width, height)
        texture.replace(region: region, mipmapLevel: 0, withBytes: stagingBuffer.contents(), bytesPerRow: bytesPerRow)

        guard let renderPass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable else {
            return
        }

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPass) else {
            return
        }

        encoder.setRenderPipelineState(pipelineState)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()
        emulator.noteFramePresented(frameId: frameId)
    }
}
