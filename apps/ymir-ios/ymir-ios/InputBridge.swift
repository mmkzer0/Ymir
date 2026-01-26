import GameController
import SwiftUI

struct ControlPadButtons: OptionSet {
    let rawValue: UInt16

    // Keep in sync with ymir_control_pad_button_t in ymir_c.h.
    static let right = ControlPadButtons(rawValue: 1 << 15)
    static let left = ControlPadButtons(rawValue: 1 << 14)
    static let down = ControlPadButtons(rawValue: 1 << 13)
    static let up = ControlPadButtons(rawValue: 1 << 12)
    static let start = ControlPadButtons(rawValue: 1 << 11)
    static let a = ControlPadButtons(rawValue: 1 << 10)
    static let c = ControlPadButtons(rawValue: 1 << 9)
    static let b = ControlPadButtons(rawValue: 1 << 8)
    static let r = ControlPadButtons(rawValue: 1 << 7)
    static let x = ControlPadButtons(rawValue: 1 << 6)
    static let y = ControlPadButtons(rawValue: 1 << 5)
    static let z = ControlPadButtons(rawValue: 1 << 4)
    static let l = ControlPadButtons(rawValue: 1 << 3)
    static let all = ControlPadButtons(rawValue: 0b1111_1111_1111_1000)
}

struct ControlPadState {
    var buttons: ControlPadButtons = []

    mutating func set(_ button: ControlPadButtons, pressed: Bool) {
        if pressed {
            buttons.insert(button)
        } else {
            buttons.remove(button)
        }
    }

    mutating func clear() {
        buttons = []
    }
}

extension EmulatorController {
    func setControlPadButtons(_ buttons: ControlPadButtons, port: UInt32 = 1) {
        setControlPadButtons(buttons.rawValue, port: port)
    }
}

final class InputCoordinator: ObservableObject {
    @Published private(set) var showTouchOverlay = true
    @Published private(set) var isControllerConnected = false

    private weak var emulator: EmulatorController?
    private var observerTokens: [NSObjectProtocol] = []
    private let stateLock = NSLock()
    private var touchState = ControlPadState()
    private var controllerState = ControlPadState()
    private weak var activeController: GCController?

    init(emulator: EmulatorController) {
        self.emulator = emulator
        startMonitoringControllers()
    }

    deinit {
        for token in observerTokens {
            NotificationCenter.default.removeObserver(token)
        }
    }

    func setTouchButton(_ button: ControlPadButtons, pressed: Bool) {
        stateLock.lock()
        touchState.set(button, pressed: pressed)
        let combined = touchState.buttons.union(controllerState.buttons)
        stateLock.unlock()
        emulator?.setControlPadButtons(combined)
    }

    func clearTouchState() {
        stateLock.lock()
        touchState.clear()
        let combined = touchState.buttons.union(controllerState.buttons)
        stateLock.unlock()
        emulator?.setControlPadButtons(combined)
    }

    private func startMonitoringControllers() {
        let center = NotificationCenter.default
        let connectToken = center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) {
            [weak self] notification in
            guard let controller = notification.object as? GCController else {
                return
            }
            self?.attachController(controller)
        }
        let disconnectToken = center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) {
            [weak self] notification in
            guard let controller = notification.object as? GCController else {
                return
            }
            self?.detachController(controller)
        }
        observerTokens = [connectToken, disconnectToken]

        if let controller = GCController.controllers().first {
            attachController(controller)
        } else {
            setControllerConnected(false)
        }
    }

    private func attachController(_ controller: GCController) {
        if let active = activeController, active !== controller {
            return
        }
        if let gamepad = controller.extendedGamepad {
            activeController = controller
            setControllerConnected(true)
            clearTouchState()
            clearControllerState()
            bindExtendedGamepad(gamepad)
        } else if let gamepad = controller.microGamepad {
            activeController = controller
            setControllerConnected(true)
            clearTouchState()
            clearControllerState()
            bindMicroGamepad(gamepad)
        } else {
            activeController = nil
            setControllerConnected(false)
            clearControllerState()
        }
    }

    private func detachController(_ controller: GCController) {
        guard activeController === controller else {
            return
        }
        activeController = nil
        setControllerConnected(false)
        clearControllerState()

        if let nextController = GCController.controllers().first {
            attachController(nextController)
        }
    }

    private func setControllerConnected(_ connected: Bool) {
        isControllerConnected = connected
        showTouchOverlay = !connected
    }

    private func bindExtendedGamepad(_ gamepad: GCExtendedGamepad) {
        bindDpad(gamepad.dpad)
        bindButton(gamepad.buttonA, to: .a)
        bindButton(gamepad.buttonB, to: .b)
        bindButton(gamepad.buttonX, to: .x)
        bindButton(gamepad.buttonY, to: .y)
        bindButton(gamepad.leftShoulder, to: .l)
        bindButton(gamepad.rightShoulder, to: .r)
        bindButton(gamepad.leftTrigger, to: .z)
        bindButton(gamepad.rightTrigger, to: .c)
        bindButton(gamepad.buttonMenu, to: .start)
    }

    private func bindMicroGamepad(_ gamepad: GCMicroGamepad) {
        bindDpad(gamepad.dpad)
        bindButton(gamepad.buttonA, to: .a)
        bindButton(gamepad.buttonX, to: .b)
        bindButton(gamepad.buttonMenu, to: .start)
    }

    private func bindDpad(_ dpad: GCControllerDirectionPad) {
        bindButton(dpad.up, to: .up)
        bindButton(dpad.down, to: .down)
        bindButton(dpad.left, to: .left)
        bindButton(dpad.right, to: .right)
    }

    private func bindButton(_ input: GCControllerButtonInput, to button: ControlPadButtons) {
        input.pressedChangedHandler = { [weak self] _, _, pressed in
            self?.setControllerButton(button, pressed: pressed)
        }
    }

    private func setControllerButton(_ button: ControlPadButtons, pressed: Bool) {
        stateLock.lock()
        controllerState.set(button, pressed: pressed)
        let combined = touchState.buttons.union(controllerState.buttons)
        stateLock.unlock()
        emulator?.setControlPadButtons(combined)
    }

    private func clearControllerState() {
        stateLock.lock()
        controllerState.clear()
        let combined = touchState.buttons.union(controllerState.buttons)
        stateLock.unlock()
        emulator?.setControlPadButtons(combined)
    }
}

struct TouchControlOverlay: View {
    @ObservedObject var input: InputCoordinator

    var body: some View {
        if input.showTouchOverlay {
            ZStack(alignment: .bottom) {
                HStack(alignment: .bottom) {
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            TouchButton(label: "L1", button: .l, input: input, size: 34)
                            Spacer()
                        }
                        DPadCluster(input: input)
                    }

                    Spacer()

                    VStack(alignment: .trailing, spacing: 12) {
                        HStack {
                            Spacer()
                            TouchButton(label: "R1", button: .r, input: input, size: 34)
                        }
                        FaceButtonCluster(input: input)
                    }
                }

                TouchButton(label: "Start", button: .start, input: input, size: 56)
                    .padding(.bottom, 6)
            }
            .padding(16)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            .transition(.opacity)
        }
    }
}

private struct DPadCluster: View {
    @ObservedObject var input: InputCoordinator

    var body: some View {
        VStack(spacing: 8) {
            TouchButton(label: "U", button: .up, input: input)
            HStack(spacing: 8) {
                TouchButton(label: "L", button: .left, input: input)
                Spacer()
                    .frame(width: 44, height: 44)
                TouchButton(label: "R", button: .right, input: input)
            }
            TouchButton(label: "D", button: .down, input: input)
        }
    }
}

private struct FaceButtonCluster: View {
    @ObservedObject var input: InputCoordinator

    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 10) {
                TouchButton(label: "X", button: .x, input: input)
                TouchButton(label: "Y", button: .y, input: input)
                TouchButton(label: "Z", button: .z, input: input)
            }
            HStack(spacing: 10) {
                TouchButton(label: "A", button: .a, input: input)
                TouchButton(label: "B", button: .b, input: input)
                TouchButton(label: "C", button: .c, input: input)
            }
        }
    }
}

private struct TouchButton: View {
    let label: String
    let button: ControlPadButtons
    @ObservedObject var input: InputCoordinator
    var size: CGFloat = 44

    @State private var isPressed = false

    var body: some View {
        Text(label)
            .font(.custom("AvenirNext-Bold", size: size * 0.32))
            .foregroundStyle(Color.white.opacity(0.9))
            .frame(width: size, height: size)
            .background(Color.white.opacity(isPressed ? 0.35 : 0.18))
            .overlay(
                Circle()
                    .stroke(Color.white.opacity(0.35), lineWidth: 1)
            )
            .clipShape(Circle())
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        setPressed(true)
                    }
                    .onEnded { _ in
                        setPressed(false)
                    }
            )
            .onDisappear {
                if isPressed {
                    setPressed(false)
                }
            }
    }

    private func setPressed(_ pressed: Bool) {
        if pressed == isPressed {
            return
        }
        isPressed = pressed
        input.setTouchButton(button, pressed: pressed)
    }
}
