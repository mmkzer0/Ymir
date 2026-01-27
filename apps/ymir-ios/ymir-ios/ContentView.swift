import SwiftUI
import UniformTypeIdentifiers

private enum AppColors {
    static let backgroundTop = Color(red: 0.97, green: 0.94, blue: 0.90)
    static let backgroundBottom = Color(red: 0.88, green: 0.93, blue: 0.98)
    static let panel = Color(red: 0.10, green: 0.12, blue: 0.14)
    static let panelStroke = Color(red: 0.20, green: 0.22, blue: 0.26)
    static let panelText = Color(red: 0.92, green: 0.92, blue: 0.90)
    static let accent = Color(red: 0.94, green: 0.52, blue: 0.27)
    static let accentSoft = Color(red: 0.96, green: 0.82, blue: 0.70)
    static let statusOk = Color(red: 0.20, green: 0.72, blue: 0.44)
    static let statusIdle = Color(red: 0.90, green: 0.64, blue: 0.22)
}

struct ContentView: View {
    @ObservedObject var emulator: EmulatorController
    @ObservedObject var logStore: LogStore

    @State private var showImporter = false
    @StateObject private var inputCoordinator: InputCoordinator

    init(emulator: EmulatorController, logStore: LogStore) {
        self.emulator = emulator
        self.logStore = logStore
        _inputCoordinator = StateObject(wrappedValue: InputCoordinator(emulator: emulator))
    }

    var body: some View {
        TabView {
            homeScreen
                .tabItem {
                    Label("Home", systemImage: "house")
                }

            videoScreen
                .tabItem {
                    Label("Video", systemImage: "rectangle.on.rectangle")
                }

            consoleScreen
                .tabItem {
                    Label("Console", systemImage: "terminal")
                }
        }
        .fileImporter(isPresented: $showImporter,
                      allowedContentTypes: [UTType.data],
                      allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first {
                    emulator.loadIPL(from: url)
                }
            case .failure(let error):
                emulator.logStore.append(level: .error, message: "File import failed: \(error.localizedDescription)")
            }
        }
    }

    private var header: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Ymir iOS")
                    .font(.custom("AvenirNext-Bold", size: 28))
                    .foregroundStyle(AppColors.panel)

                Text("Saturn IPL boot log")
                    .font(.custom("AvenirNext-Regular", size: 14))
                    .foregroundStyle(Color.black.opacity(0.55))
            }

            Spacer()

            VStack(alignment: .trailing, spacing: 6) {
                Text(emulator.statusText)
                    .font(.custom("AvenirNext-DemiBold", size: 13))
                    .foregroundStyle(Color.black.opacity(0.6))

                HStack(spacing: 6) {
                    Circle()
                        .fill(emulator.isRunning ? AppColors.statusOk : AppColors.statusIdle)
                        .frame(width: 8, height: 8)
                    Text(emulator.isRunning ? "Running" : "Idle")
                        .font(.custom("AvenirNext-DemiBold", size: 12))
                        .foregroundStyle(Color.black.opacity(0.7))
                }
            }
        }
    }

    private var statusCard: some View {
        HStack(alignment: .center, spacing: 16) {
            VStack(alignment: .leading, spacing: 6) {
                Text("IPL ROM")
                    .font(.custom("AvenirNext-DemiBold", size: 12))
                    .foregroundStyle(Color.black.opacity(0.6))
                Text(emulator.iplName)
                    .font(.custom("AvenirNext-Bold", size: 15))
                    .foregroundStyle(AppColors.panel)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer()

            Button {
                showImporter = true
            } label: {
                Text("Load IPL")
                    .font(.custom("AvenirNext-DemiBold", size: 13))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(AppColors.accentSoft)
                    .foregroundStyle(Color.black.opacity(0.8))
                    .clipShape(Capsule())
            }
        }
        .padding(14)
        .background(Color.white.opacity(0.8))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    private var controls: some View {
        HStack(spacing: 12) {
            Button {
                if emulator.isRunning {
                    emulator.stop()
                } else {
                    emulator.start()
                }
            } label: {
                Text(emulator.isRunning ? "Stop" : "Start")
                    .font(.custom("AvenirNext-Bold", size: 14))
                    .padding(.horizontal, 18)
                    .padding(.vertical, 10)
                    .foregroundStyle(Color.white)
                    .background(AppColors.accent)
                    .clipShape(Capsule())
            }

            Button {
                emulator.logStore.clear()
            } label: {
                Text("Clear Logs")
                    .font(.custom("AvenirNext-DemiBold", size: 13))
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .foregroundStyle(Color.black.opacity(0.7))
                    .background(Color.white.opacity(0.9))
                    .clipShape(Capsule())
            }

            Spacer()
        }
    }

    private var logPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Console")
                .font(.custom("AvenirNext-DemiBold", size: 12))
                .foregroundStyle(AppColors.panelText.opacity(0.7))

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(logStore.entries) { entry in
                            LogRow(entry: entry)
                                .id(entry.id)
                        }
                    }
                    .padding(.vertical, 6)
                }
                .onChange(of: logStore.entries.count) { _ in
                    if let lastId = logStore.entries.last?.id {
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo(lastId, anchor: .bottom)
                        }
                    }
                }
            }
        }
        .padding(14)
        .background(AppColors.panel)
        .overlay(
            RoundedRectangle(cornerRadius: 16)
                .stroke(AppColors.panelStroke, lineWidth: 1)
        )
        .clipShape(RoundedRectangle(cornerRadius: 16))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private extension ContentView {
    var homeScreen: some View {
        screenContainer {
            VStack(alignment: .leading, spacing: 16) {
                header
                statusCard
                controls
                audioPanel
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }

    var videoScreen: some View {
        screenContainer {
            VStack(alignment: .leading, spacing: 16) {
                header
                statusCard
                videoPanel
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }

    var consoleScreen: some View {
        screenContainer {
            VStack(alignment: .leading, spacing: 16) {
                header
                statusCard
                logPanel
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }

    func screenContainer<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        ZStack {
            LinearGradient(colors: [AppColors.backgroundTop, AppColors.backgroundBottom],
                           startPoint: .topLeading,
                           endPoint: .bottomTrailing)
                .ignoresSafeArea()

            content()
                .padding(20)
        }
    }

    var audioPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Audio")
                        .font(.custom("AvenirNext-DemiBold", size: 12))
                        .foregroundStyle(Color.black.opacity(0.6))
                    Text(emulator.audioEnabled ? "Enabled" : "Muted")
                        .font(.custom("AvenirNext-Bold", size: 14))
                        .foregroundStyle(AppColors.panel)
                }

                Spacer()

                Toggle("", isOn: Binding(get: {
                    emulator.audioEnabled
                }, set: { value in
                    emulator.setAudioEnabled(value)
                }))
                .labelsHidden()
                .toggleStyle(SwitchToggleStyle(tint: AppColors.accent))
            }

            AudioMeter(fill: emulator.audioBufferFill,
                       queued: emulator.audioQueuedFrames,
                       capacity: emulator.audioCapacityFrames)
        }
        .padding(14)
        .background(Color.white.opacity(0.8))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    var videoPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Video")
                .font(.custom("AvenirNext-DemiBold", size: 12))
                .foregroundStyle(AppColors.panelText.opacity(0.7))

            ZStack {
                MetalFramebufferView(emulator: emulator)
                TouchControlOverlay(input: inputCoordinator)
                    .allowsHitTesting(inputCoordinator.showTouchOverlay)
                    .animation(.easeInOut(duration: 0.2), value: inputCoordinator.showTouchOverlay)
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .frame(maxWidth: .infinity)
            .background(Color.black.opacity(0.9))
            .clipShape(RoundedRectangle(cornerRadius: 12))
        }
        .padding(14)
        .background(AppColors.panel)
        .overlay(
            RoundedRectangle(cornerRadius: 16)
                .stroke(AppColors.panelStroke, lineWidth: 1)
        )
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }
}

private struct LogRow: View {
    let entry: LogEntry

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Text(entry.level.label)
                .font(.custom("Menlo", size: 10))
                .foregroundStyle(entry.level.color)
                .frame(width: 56, alignment: .leading)

            Text(entry.timestamp, style: .time)
                .font(.custom("Menlo", size: 10))
                .foregroundStyle(AppColors.panelText.opacity(0.6))
                .frame(width: 58, alignment: .leading)

            Text(entry.message)
                .font(.custom("Menlo", size: 11))
                .foregroundStyle(AppColors.panelText)
                .lineLimit(nil)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct AudioMeter: View {
    let fill: Double
    let queued: Int
    let capacity: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            GeometryReader { geo in
                let clampedFill = min(max(fill, 0.0), 1.0)
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Color.black.opacity(0.08))
                    RoundedRectangle(cornerRadius: 4)
                        .fill(AppColors.accent)
                        .frame(width: geo.size.width * CGFloat(clampedFill))
                }
            }
            .frame(height: 8)

            Text(capacity > 0 ? "\(queued)/\(capacity) frames" : "No audio data")
                .font(.custom("AvenirNext-Regular", size: 11))
                .foregroundStyle(Color.black.opacity(0.5))
        }
    }
}
