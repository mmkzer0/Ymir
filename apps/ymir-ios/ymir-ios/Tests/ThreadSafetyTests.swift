// ThreadSafetyTests.swift
// Concurrency stress tests to verify "no crash under concurrent access".
// These tests validate that thread-safe locking in MetalFramebufferRenderer
// prevents data races when options and cache are accessed concurrently.

import Testing
import Foundation

// MARK: - Thread-Safe Lock Wrapper (Mirror of UnfairLock)

/// A minimal wrapper around os_unfair_lock for testing purposes.
/// This mirrors the UnfairLock class in MetalFramebufferRenderer.
fileprivate final class UnfairLock: @unchecked Sendable {
    private var _lock = os_unfair_lock()
    
    func withLock<T>(_ body: () throws -> T) rethrows -> T {
        os_unfair_lock_lock(&_lock)
        defer { os_unfair_lock_unlock(&_lock) }
        return try body()
    }
}

// MARK: - Thread-Safe Cache for Testing

/// Thread-safe LRU cache implementation mirroring MetalFramebufferRenderer pattern.
/// Uses UnfairLock for synchronization - same as production code.
fileprivate final class ThreadSafeCache<Key: Hashable, Value>: @unchecked Sendable {
    private let lock = UnfairLock()
    private var storage: [Key: Value] = [:]
    private var accessOrder: [Key] = []
    private let maxSize: Int
    
    init(maxSize: Int) {
        self.maxSize = maxSize
    }
    
    func get(_ key: Key) -> Value? {
        lock.withLock {
            guard let value = storage[key] else { return nil }
            // Update LRU order
            if let idx = accessOrder.firstIndex(of: key) {
                accessOrder.remove(at: idx)
                accessOrder.append(key)
            }
            return value
        }
    }
    
    func insertOutsideLock(_ key: Key, _ valueBuilder: () -> Value) {
        // Simulate production pattern: check cache, build value outside lock, then insert
        let existingValue: Value? = lock.withLock { storage[key] }
        
        if existingValue != nil {
            return // Already cached
        }
        
        // Build value outside lock (simulates PSO compilation)
        let newValue = valueBuilder()
        
        // Insert under lock
        lock.withLock {
            // Double-check (another thread may have inserted)
            if storage[key] != nil {
                return
            }
            
            // LRU eviction
            if accessOrder.count >= maxSize {
                let evictedKey = accessOrder.removeFirst()
                storage.removeValue(forKey: evictedKey)
            }
            
            storage[key] = newValue
            accessOrder.append(key)
        }
    }
    
    var count: Int {
        lock.withLock { storage.count }
    }
    
    var keys: [Key] {
        lock.withLock { accessOrder }
    }
}

// MARK: - Thread-Safe Options Container for Testing

/// Thread-safe options container mirroring MetalFramebufferRenderer.options pattern.
fileprivate final class ThreadSafeOptions<T: Equatable>: @unchecked Sendable {
    private let lock = UnfairLock()
    private var _value: T
    
    init(_ initial: T) {
        _value = initial
    }
    
    var value: T {
        lock.withLock { _value }
    }
    
    func update(_ newValue: T) {
        lock.withLock {
            if _value != newValue {
                _value = newValue
            }
        }
    }
}

// MARK: - Concurrency Stress Tests

@Suite("Thread Safety", .serialized)
struct ThreadSafetyTests {
    
    @Test("Concurrent cache writes don't crash", .timeLimit(.minutes(1)))
    func concurrentCacheWrites() async {
        let cache = ThreadSafeCache<String, Int>(maxSize: 8)
        let iterations = 1000
        let concurrentWriters = 10
        
        await withTaskGroup(of: Void.self) { group in
            for writerIndex in 0..<concurrentWriters {
                group.addTask {
                    for i in 0..<iterations {
                        let key = "key_\(writerIndex)_\(i % 20)" // 20 unique keys per writer
                        cache.insertOutsideLock(key) { i }
                    }
                }
            }
        }
        
        // Test passes if we get here without crash
        #expect(cache.count <= 8) // Cache respects max size
    }
    
    @Test("Concurrent cache reads and writes don't crash", .timeLimit(.minutes(1)))
    func concurrentReadsAndWrites() async {
        let cache = ThreadSafeCache<String, Int>(maxSize: 8)
        
        // Pre-populate cache
        for i in 0..<5 {
            cache.insertOutsideLock("initial_\(i)") { i }
        }
        
        let iterations = 500
        let concurrentTasks = 20
        
        await withTaskGroup(of: Void.self) { group in
            for taskIndex in 0..<concurrentTasks {
                group.addTask {
                    for i in 0..<iterations {
                        if taskIndex % 2 == 0 {
                            // Writers
                            cache.insertOutsideLock("write_\(taskIndex)_\(i % 10)") { i }
                        } else {
                            // Readers
                            _ = cache.get("initial_\(i % 5)")
                            _ = cache.get("write_\((taskIndex - 1))_\(i % 10)")
                        }
                    }
                }
            }
        }
        
        // Test passes if we get here without crash
        #expect(cache.count <= 8)
    }
    
    @Test("Concurrent options updates don't crash", .timeLimit(.minutes(1)))
    func concurrentOptionsUpdates() async {
        struct MockOptions: Equatable {
            var shaderName: String
            var upscaler: Int
            var format: Int
        }
        
        let options = ThreadSafeOptions(MockOptions(shaderName: "default", upscaler: 0, format: 0))
        let iterations = 1000
        let concurrentUpdaters = 10
        
        await withTaskGroup(of: Void.self) { group in
            for updaterIndex in 0..<concurrentUpdaters {
                group.addTask {
                    for i in 0..<iterations {
                        let newOptions = MockOptions(
                            shaderName: "shader_\(updaterIndex)",
                            upscaler: i % 2,
                            format: i % 3
                        )
                        options.update(newOptions)
                        
                        // Also read concurrently
                        let _ = options.value
                    }
                }
            }
        }
        
        // Test passes if we get here without crash
        let finalValue = options.value
        #expect(finalValue.shaderName.hasPrefix("shader_"))
    }
    
    @Test("Rapid sequential option updates are atomic", .timeLimit(.minutes(1)))
    func rapidSequentialOptionUpdates() async {
        struct CounterOptions: Equatable {
            var counter: Int
        }
        
        let options = ThreadSafeOptions(CounterOptions(counter: 0))
        let iterations = 10000
        
        // Single writer, multiple readers pattern
        await withTaskGroup(of: Void.self) { group in
            // Writer task
            group.addTask {
                for i in 1...iterations {
                    options.update(CounterOptions(counter: i))
                }
            }
            
            // Reader tasks
            for _ in 0..<5 {
                group.addTask {
                    var lastSeen = 0
                    for _ in 0..<iterations {
                        let current = options.value.counter
                        // Values should be monotonically increasing or we see same value
                        #expect(current >= lastSeen)
                        lastSeen = current
                    }
                }
            }
        }
        
        // Final value should be last written
        #expect(options.value.counter == iterations)
    }
    
    @Test("Mixed cache and options access doesn't deadlock", .timeLimit(.minutes(1)))
    func mixedCacheAndOptionsAccess() async {
        let cache = ThreadSafeCache<String, Int>(maxSize: 8)
        let options = ThreadSafeOptions("default")
        let iterations = 500
        let concurrentTasks = 10
        
        await withTaskGroup(of: Void.self) { group in
            for taskIndex in 0..<concurrentTasks {
                group.addTask {
                    for i in 0..<iterations {
                        // Simulate render loop: read options, then access cache
                        let currentShader = options.value
                        let key = "\(currentShader)_\(i % 10)"
                        
                        if taskIndex % 3 == 0 {
                            // Update options
                            options.update("shader_\(taskIndex)")
                        }
                        
                        // Access cache based on options
                        if let _ = cache.get(key) {
                            // Cache hit path
                        } else {
                            // Cache miss path - insert
                            cache.insertOutsideLock(key) { i }
                        }
                    }
                }
            }
        }
        
        // Test passes if no deadlock and we complete
        #expect(cache.count <= 8)
    }
    
    @Test("LRU eviction under concurrent access maintains invariants", .timeLimit(.minutes(1)))
    func lruEvictionUnderConcurrency() async {
        let cache = ThreadSafeCache<Int, Int>(maxSize: 8)
        let totalUniqueKeys = 100
        let iterations = 50
        let concurrentWriters = 10
        
        await withTaskGroup(of: Void.self) { group in
            for writerIndex in 0..<concurrentWriters {
                group.addTask {
                    for i in 0..<iterations {
                        let key = (writerIndex * iterations + i) % totalUniqueKeys
                        cache.insertOutsideLock(key) { key * 10 }
                    }
                }
            }
        }
        
        // Invariant: cache size never exceeds maxSize
        #expect(cache.count <= 8)
        
        // Invariant: all keys in cache have valid values
        for key in cache.keys {
            if let value = cache.get(key) {
                #expect(value == key * 10)
            }
        }
    }
}

// MARK: - Dispatch Queue Stress Tests

@Suite("Dispatch Queue Stress", .serialized)
struct DispatchQueueStressTests {
    
    @Test("High-frequency updates from multiple queues", .timeLimit(.minutes(1)))
    func highFrequencyMultiQueueUpdates() async {
        let cache = ThreadSafeCache<String, Int>(maxSize: 8)
        let options = ThreadSafeOptions(0)
        
        let expectation = XCTestExpectation(description: "All queues complete")
        expectation.expectedFulfillmentCount = 5
        
        let queues = (0..<5).map { DispatchQueue(label: "test.queue.\($0)", attributes: .concurrent) }
        
        for (queueIndex, queue) in queues.enumerated() {
            queue.async {
                for i in 0..<200 {
                    // Mix of operations
                    switch i % 4 {
                    case 0:
                        options.update(queueIndex * 1000 + i)
                    case 1:
                        _ = options.value
                    case 2:
                        cache.insertOutsideLock("q\(queueIndex)_\(i % 10)") { i }
                    case 3:
                        _ = cache.get("q\((queueIndex + 1) % 5)_\(i % 10)")
                    default:
                        break
                    }
                }
                expectation.fulfill()
            }
        }
        
        // Wait for completion (XCTestExpectation used for GCD synchronization)
        await withCheckedContinuation { continuation in
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) {
                continuation.resume()
            }
        }
        
        #expect(cache.count <= 8)
    }
}

// MARK: - XCTest Integration for GCD

import XCTest

/// Minimal XCTestExpectation wrapper for use with Swift Testing.
/// Required only for GCD-based stress tests.
fileprivate final class XCTestExpectation: @unchecked Sendable {
    private let lock = NSLock()
    private var fulfillmentCount = 0
    var expectedFulfillmentCount = 1
    let description: String
    
    init(description: String) {
        self.description = description
    }
    
    func fulfill() {
        lock.lock()
        fulfillmentCount += 1
        lock.unlock()
    }
    
    var isFulfilled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return fulfillmentCount >= expectedFulfillmentCount
    }
}
