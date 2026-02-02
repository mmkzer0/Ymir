// OptionsTests.swift
// Pure-logic tests for FrameProcessingOptions, ShaderSelection, and Upscaler types.
// Validates equality semantics, default values, and hashability.

import Testing
import Metal

// MARK: - Type Mirrors for Pure-Logic Testing

/// Mirror of MetalFramebufferView.Upscaler
fileprivate enum Upscaler: Equatable {
    case none
    case metalFXSpatial
}

/// Mirror of MetalFramebufferView.ShaderSelection
fileprivate struct ShaderSelection: Hashable {
    var fragmentFunctionName: String
    var label: String
    
    static let `default` = ShaderSelection(fragmentFunctionName: "ymir_fragment", label: "Default")
}

/// Mirror of MetalFramebufferView.FrameProcessingOptions
fileprivate struct FrameProcessingOptions: Equatable {
    var upscaler: Upscaler
    var shader: ShaderSelection
    var drawablePixelFormat: MTLPixelFormat
    
    static let `default` = FrameProcessingOptions(
        upscaler: .none,
        shader: .default,
        drawablePixelFormat: .rgba8Unorm
    )
}

// MARK: - Shader Selection Tests

@Suite("ShaderSelection")
struct ShaderSelectionTests {
    
    @Test("Default shader has expected fragment function name")
    func defaultShaderFragmentFunction() {
        let shader = ShaderSelection.default
        #expect(shader.fragmentFunctionName == "ymir_fragment")
        #expect(shader.label == "Default")
    }
    
    @Test("ShaderSelection equality works correctly")
    func shaderEquality() {
        let shader1 = ShaderSelection(fragmentFunctionName: "crt_shader", label: "CRT")
        let shader2 = ShaderSelection(fragmentFunctionName: "crt_shader", label: "CRT")
        let shader3 = ShaderSelection(fragmentFunctionName: "crt_shader", label: "CRT Effect")
        
        #expect(shader1 == shader2)
        #expect(shader1 != shader3) // Different label
    }
    
    @Test("ShaderSelection can be used as Set element")
    func shaderAsSetElement() {
        var shaders: Set<ShaderSelection> = []
        
        let shader1 = ShaderSelection(fragmentFunctionName: "a", label: "A")
        let shader2 = ShaderSelection(fragmentFunctionName: "b", label: "B")
        let shader3 = ShaderSelection(fragmentFunctionName: "a", label: "A") // duplicate
        
        shaders.insert(shader1)
        shaders.insert(shader2)
        shaders.insert(shader3)
        
        #expect(shaders.count == 2) // shader3 is duplicate of shader1
    }
    
    @Test("ShaderSelection can be used as Dictionary key")
    func shaderAsDictionaryKey() {
        var psoMap: [ShaderSelection: String] = [:]
        
        let shader1 = ShaderSelection(fragmentFunctionName: "default", label: "Default")
        let shader2 = ShaderSelection(fragmentFunctionName: "crt", label: "CRT")
        
        psoMap[shader1] = "PSO_DEFAULT"
        psoMap[shader2] = "PSO_CRT"
        
        #expect(psoMap[shader1] == "PSO_DEFAULT")
        
        // Same value should lookup correctly
        let lookupKey = ShaderSelection(fragmentFunctionName: "default", label: "Default")
        #expect(psoMap[lookupKey] == "PSO_DEFAULT")
    }
}

// MARK: - Upscaler Tests

@Suite("Upscaler")
struct UpscalerTests {
    
    @Test("Upscaler cases are distinct")
    func upscalerCases() {
        let none = Upscaler.none
        let metalFX = Upscaler.metalFXSpatial
        
        #expect(none != metalFX)
        #expect(none == .none)
        #expect(metalFX == .metalFXSpatial)
    }
}

// MARK: - Frame Processing Options Tests

@Suite("FrameProcessingOptions")
struct FrameProcessingOptionsTests {
    
    @Test("Default options have expected values")
    func defaultOptionsValues() {
        let options = FrameProcessingOptions.default
        
        #expect(options.upscaler == .none)
        #expect(options.shader == .default)
        #expect(options.drawablePixelFormat == .rgba8Unorm)
    }
    
    @Test("Equal options compare equal")
    func optionsEquality() {
        let options1 = FrameProcessingOptions(
            upscaler: .none,
            shader: ShaderSelection(fragmentFunctionName: "test", label: "Test"),
            drawablePixelFormat: .rgba8Unorm
        )
        let options2 = FrameProcessingOptions(
            upscaler: .none,
            shader: ShaderSelection(fragmentFunctionName: "test", label: "Test"),
            drawablePixelFormat: .rgba8Unorm
        )
        
        #expect(options1 == options2)
    }
    
    @Test("Options with different upscaler are unequal")
    func optionsInequalityByUpscaler() {
        let options1 = FrameProcessingOptions(
            upscaler: .none,
            shader: .default,
            drawablePixelFormat: .rgba8Unorm
        )
        let options2 = FrameProcessingOptions(
            upscaler: .metalFXSpatial,
            shader: .default,
            drawablePixelFormat: .rgba8Unorm
        )
        
        #expect(options1 != options2)
    }
    
    @Test("Options with different shader are unequal")
    func optionsInequalityByShader() {
        let options1 = FrameProcessingOptions(
            upscaler: .none,
            shader: ShaderSelection(fragmentFunctionName: "a", label: "A"),
            drawablePixelFormat: .rgba8Unorm
        )
        let options2 = FrameProcessingOptions(
            upscaler: .none,
            shader: ShaderSelection(fragmentFunctionName: "b", label: "B"),
            drawablePixelFormat: .rgba8Unorm
        )
        
        #expect(options1 != options2)
    }
    
    @Test("Options with different pixel format are unequal")
    func optionsInequalityByPixelFormat() {
        let options1 = FrameProcessingOptions(
            upscaler: .none,
            shader: .default,
            drawablePixelFormat: .rgba8Unorm
        )
        let options2 = FrameProcessingOptions(
            upscaler: .none,
            shader: .default,
            drawablePixelFormat: .bgra8Unorm
        )
        
        #expect(options1 != options2)
    }
    
    @Test("Option changes affect cache key generation")
    func optionChangesAffectCacheKey() {
        // Simulating cache key generation logic from MetalFramebufferRenderer
        struct PipelineKey: Hashable {
            let fragmentFunctionName: String
            let pixelFormat: MTLPixelFormat
        }
        
        func keyFromOptions(_ options: FrameProcessingOptions) -> PipelineKey {
            PipelineKey(
                fragmentFunctionName: options.shader.fragmentFunctionName,
                pixelFormat: options.drawablePixelFormat
            )
        }
        
        let options1 = FrameProcessingOptions.default
        let options2 = FrameProcessingOptions(
            upscaler: .metalFXSpatial, // different upscaler
            shader: .default,
            drawablePixelFormat: .rgba8Unorm
        )
        let options3 = FrameProcessingOptions(
            upscaler: .none,
            shader: ShaderSelection(fragmentFunctionName: "crt", label: "CRT"), // different shader
            drawablePixelFormat: .rgba8Unorm
        )
        
        let key1 = keyFromOptions(options1)
        let key2 = keyFromOptions(options2)
        let key3 = keyFromOptions(options3)
        
        // Same shader + format = same key (upscaler doesn't affect PSO key)
        #expect(key1 == key2)
        
        // Different shader = different key
        #expect(key1 != key3)
    }
}

// MARK: - Pixel Format Mapping Tests

@Suite("Pixel Format Mapping")
struct PixelFormatMappingTests {
    
    /// Mirror of framebufferPixelFormat(for:) logic
    enum MockFramebufferFormat: UInt32 {
        case xbgr8888 = 0
        case unknown = 999
    }
    
    func framebufferPixelFormat(for format: MockFramebufferFormat) -> MTLPixelFormat {
        switch format {
        case .xbgr8888:
            // Little-endian XBGR8888 means bytes are RGBA in memory
            return .rgba8Unorm
        default:
            return .rgba8Unorm
        }
    }
    
    @Test("XBGR8888 maps to rgba8Unorm")
    func xbgr8888MapsToRgba8Unorm() {
        let result = framebufferPixelFormat(for: .xbgr8888)
        #expect(result == .rgba8Unorm)
    }
    
    @Test("Unknown format falls back to rgba8Unorm")
    func unknownFormatDefaultsToRgba8Unorm() {
        let result = framebufferPixelFormat(for: .unknown)
        #expect(result == .rgba8Unorm)
    }
}
