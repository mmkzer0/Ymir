import SwiftUI

@main
struct YmirIOSApp: App {
    @StateObject private var emulator = EmulatorController()

    var body: some Scene {
        WindowGroup {
            ContentView(emulator: emulator, logStore: emulator.logStore)
        }
    }
}
