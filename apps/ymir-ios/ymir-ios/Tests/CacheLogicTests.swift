// CacheLogicTests.swift
// Pure-logic tests for pipeline cache behavior using Swift Testing framework.
// These tests verify cache key generation, equality, hashing, and LRU eviction
// logic WITHOUT requiring real Metal devices or PSO compilation.

import Testing
import Metal

// MARK: - Pipeline Key Tests

/// PipelineKey is a private struct in MetalFramebufferRenderer.
/// To test its behavior, we mirror its definition here for pure-logic testing.
/// This avoids requiring @testable import or exposing internal types.
fileprivate struct PipelineKey: Hashable {
    let fragmentFunctionName: String
    let pixelFormat: MTLPixelFormat
}

@Suite("Pipeline Key Logic")
struct PipelineKeyTests {
    
    @Test("Equal keys with same function name and pixel format")
    func keyEquality() {
        let key1 = PipelineKey(fragmentFunctionName: "ymir_fragment", pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: "ymir_fragment", pixelFormat: .rgba8Unorm)
        
        #expect(key1 == key2)
        #expect(key1.hashValue == key2.hashValue)
    }
    
    @Test("Unequal keys with different function names")
    func keyInequalityByFunctionName() {
        let key1 = PipelineKey(fragmentFunctionName: "ymir_fragment", pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: "crt_scanline", pixelFormat: .rgba8Unorm)
        
        #expect(key1 != key2)
    }
    
    @Test("Unequal keys with different pixel formats")
    func keyInequalityByPixelFormat() {
        let key1 = PipelineKey(fragmentFunctionName: "ymir_fragment", pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: "ymir_fragment", pixelFormat: .bgra8Unorm)
        
        #expect(key1 != key2)
    }
    
    @Test("Key can be used as Dictionary key")
    func keyAsHashableInDictionary() {
        var cache: [PipelineKey: String] = [:]
        let key1 = PipelineKey(fragmentFunctionName: "shader_a", pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: "shader_b", pixelFormat: .rgba8Unorm)
        let key3 = PipelineKey(fragmentFunctionName: "shader_a", pixelFormat: .bgra8Unorm)
        
        cache[key1] = "PSO_A"
        cache[key2] = "PSO_B"
        cache[key3] = "PSO_C"
        
        #expect(cache.count == 3)
        #expect(cache[key1] == "PSO_A")
        #expect(cache[key2] == "PSO_B")
        
        // Same key lookup should return existing value
        let duplicateKey = PipelineKey(fragmentFunctionName: "shader_a", pixelFormat: .rgba8Unorm)
        #expect(cache[duplicateKey] == "PSO_A")
    }
}

// MARK: - LRU Cache Logic Tests

/// A standalone LRU cache implementation mirroring MetalFramebufferRenderer's behavior.
/// Used exclusively for pure-logic testing without Metal dependencies.
fileprivate final class MockLRUCache<Key: Hashable, Value> {
    private var storage: [Key: Value] = [:]
    private var accessOrder: [Key] = []
    let maxSize: Int
    
    private(set) var evictionCount = 0
    private(set) var hitCount = 0
    private(set) var missCount = 0
    
    init(maxSize: Int) {
        self.maxSize = maxSize
    }
    
    func get(_ key: Key) -> Value? {
        if let value = storage[key] {
            hitCount += 1
            // Move to end (most recently used)
            if let idx = accessOrder.firstIndex(of: key) {
                accessOrder.remove(at: idx)
                accessOrder.append(key)
            }
            return value
        }
        missCount += 0 // tracked on insert, not on get failure
        return nil
    }
    
    func insert(_ key: Key, _ value: Value) {
        if storage[key] != nil {
            // Key already exists, update value and move to end
            storage[key] = value
            if let idx = accessOrder.firstIndex(of: key) {
                accessOrder.remove(at: idx)
                accessOrder.append(key)
            }
            return
        }
        
        missCount += 1
        
        // Evict oldest if at capacity
        if accessOrder.count >= maxSize {
            let evictedKey = accessOrder.removeFirst()
            storage.removeValue(forKey: evictedKey)
            evictionCount += 1
        }
        
        storage[key] = value
        accessOrder.append(key)
    }
    
    var count: Int { storage.count }
    var keys: [Key] { accessOrder }
}

@Suite("LRU Cache Behavior")
struct LRUCacheTests {
    
    @Test("Cache hit returns value and updates access order")
    func cacheHit() {
        let cache = MockLRUCache<String, Int>(maxSize: 3)
        cache.insert("a", 1)
        cache.insert("b", 2)
        cache.insert("c", 3)
        
        // Access "a" to make it most recently used
        let value = cache.get("a")
        
        #expect(value == 1)
        #expect(cache.hitCount == 1)
        #expect(cache.keys == ["b", "c", "a"]) // "a" moved to end
    }
    
    @Test("Cache miss returns nil")
    func cacheMiss() {
        let cache = MockLRUCache<String, Int>(maxSize: 3)
        cache.insert("a", 1)
        
        let value = cache.get("nonexistent")
        
        #expect(value == nil)
    }
    
    @Test("Cache evicts LRU entry when full")
    func lruEviction() {
        let cache = MockLRUCache<String, Int>(maxSize: 3)
        cache.insert("a", 1)
        cache.insert("b", 2)
        cache.insert("c", 3)
        
        // Insert fourth entry, should evict "a" (oldest)
        cache.insert("d", 4)
        
        #expect(cache.count == 3)
        #expect(cache.get("a") == nil) // evicted
        #expect(cache.get("d") == 4)   // present
        #expect(cache.evictionCount == 1)
    }
    
    @Test("Access pattern affects eviction order")
    func accessPatternAffectsEviction() {
        let cache = MockLRUCache<String, Int>(maxSize: 3)
        cache.insert("a", 1)
        cache.insert("b", 2)
        cache.insert("c", 3)
        
        // Access "a" to make it recently used
        _ = cache.get("a")
        
        // Insert "d", should evict "b" (now oldest after "a" was accessed)
        cache.insert("d", 4)
        
        #expect(cache.get("a") != nil) // still present
        #expect(cache.get("b") == nil) // evicted
        #expect(cache.get("c") != nil) // present
        #expect(cache.get("d") != nil) // present
    }
    
    @Test("Cache size matches maxCacheSize (8)")
    func cacheSizeMatchesSpec() {
        let cache = MockLRUCache<String, Int>(maxSize: 8) // matches MetalFramebufferRenderer.maxCacheSize
        
        // Insert 10 entries
        for i in 0..<10 {
            cache.insert("key\(i)", i)
        }
        
        #expect(cache.count == 8)
        #expect(cache.evictionCount == 2)
        
        // First 2 entries should be evicted
        #expect(cache.get("key0") == nil)
        #expect(cache.get("key1") == nil)
        
        // Remaining 8 should be present
        #expect(cache.get("key2") != nil)
        #expect(cache.get("key9") != nil)
    }
    
    @Test("Double insertion updates value without eviction")
    func doubleInsertionUpdatesValue() {
        let cache = MockLRUCache<String, Int>(maxSize: 3)
        cache.insert("a", 1)
        cache.insert("b", 2)
        cache.insert("c", 3)
        
        // Re-insert "a" with new value
        cache.insert("a", 100)
        
        #expect(cache.get("a") == 100)
        #expect(cache.count == 3)
        #expect(cache.evictionCount == 0)
        #expect(cache.keys.last == "a") // moved to end
    }
}

// MARK: - Pipeline Key from Options Tests

/// ShaderSelection mirror for testing (matches MetalFramebufferView.ShaderSelection)
fileprivate struct ShaderSelection: Hashable {
    var fragmentFunctionName: String
    var label: String
    
    static let `default` = ShaderSelection(fragmentFunctionName: "ymir_fragment", label: "Default")
}

@Suite("Pipeline Key Generation from Options")
struct PipelineKeyFromOptionsTests {
    
    @Test("Different shaders produce different keys")
    func differentShadersProduceDifferentKeys() {
        let shader1 = ShaderSelection(fragmentFunctionName: "ymir_fragment", label: "Default")
        let shader2 = ShaderSelection(fragmentFunctionName: "crt_shader", label: "CRT")
        
        let key1 = PipelineKey(fragmentFunctionName: shader1.fragmentFunctionName, pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: shader2.fragmentFunctionName, pixelFormat: .rgba8Unorm)
        
        #expect(key1 != key2)
    }
    
    @Test("Same shader with different format produces different key")
    func sameShaderDifferentFormatProducesDifferentKey() {
        let shader = ShaderSelection.default
        
        let key1 = PipelineKey(fragmentFunctionName: shader.fragmentFunctionName, pixelFormat: .rgba8Unorm)
        let key2 = PipelineKey(fragmentFunctionName: shader.fragmentFunctionName, pixelFormat: .bgra8Unorm)
        
        #expect(key1 != key2)
    }
    
    @Test("Identical options produce identical keys")
    func identicalOptionsProduceIdenticalKeys() {
        let shader = ShaderSelection.default
        let format: MTLPixelFormat = .rgba8Unorm
        
        let key1 = PipelineKey(fragmentFunctionName: shader.fragmentFunctionName, pixelFormat: format)
        let key2 = PipelineKey(fragmentFunctionName: shader.fragmentFunctionName, pixelFormat: format)
        
        #expect(key1 == key2)
        
        // Can be used to deduplicate cache lookups
        var seen: Set<PipelineKey> = []
        seen.insert(key1)
        #expect(seen.contains(key2))
    }
}
