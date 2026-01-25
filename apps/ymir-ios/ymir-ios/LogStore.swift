import Foundation
import SwiftUI

enum LogLevel: Int {
    case trace = 1
    case debug = 2
    case info = 3
    case warn = 4
    case error = 5

    init(cLevel: ymir_log_level_t) {
        self = LogLevel(rawValue: Int(cLevel.rawValue)) ?? .info
    }

    var label: String {
        switch self {
        case .trace:
            return "TRACE"
        case .debug:
            return "DEBUG"
        case .info:
            return "INFO"
        case .warn:
            return "WARN"
        case .error:
            return "ERROR"
        }
    }

    var color: Color {
        switch self {
        case .trace:
            return Color(red: 0.62, green: 0.66, blue: 0.70)
        case .debug:
            return Color(red: 0.28, green: 0.53, blue: 0.90)
        case .info:
            return Color(red: 0.16, green: 0.63, blue: 0.40)
        case .warn:
            return Color(red: 0.92, green: 0.63, blue: 0.20)
        case .error:
            return Color(red: 0.86, green: 0.25, blue: 0.23)
        }
    }
}

struct LogEntry: Identifiable, Hashable {
    let id = UUID()
    let timestamp: Date
    let level: LogLevel
    let message: String
}

final class LogStore: ObservableObject {
    @Published private(set) var entries: [LogEntry] = []

    private let maxEntries = 600
    private let maxPendingEntries = 1200
    private let pendingLock = NSLock()
    private var pendingEntries: [LogEntry] = []
    private var flushTimer: DispatchSourceTimer?

    init() {
        startFlusher()
    }

    func append(level: LogLevel, message: String) {
        let entry = LogEntry(timestamp: Date(), level: level, message: message)
        pendingLock.lock()
        pendingEntries.append(entry)
        if pendingEntries.count > maxPendingEntries {
            pendingEntries.removeFirst(pendingEntries.count - maxPendingEntries)
        }
        pendingLock.unlock()
    }

    func clear() {
        pendingLock.lock()
        pendingEntries.removeAll()
        pendingLock.unlock()
        if Thread.isMainThread {
            entries.removeAll()
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.entries.removeAll()
            }
        }
    }

    private func startFlusher() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.main)
        timer.schedule(deadline: .now() + 0.1, repeating: 0.1)
        timer.setEventHandler { [weak self] in
            self?.flushPending()
        }
        timer.resume()
        flushTimer = timer
    }

    private func flushPending() {
        pendingLock.lock()
        if pendingEntries.isEmpty {
            pendingLock.unlock()
            return
        }
        let drained = pendingEntries
        pendingEntries.removeAll()
        pendingLock.unlock()

        entries.append(contentsOf: drained)
        if entries.count > maxEntries {
            entries.removeFirst(entries.count - maxEntries)
        }
    }
}
