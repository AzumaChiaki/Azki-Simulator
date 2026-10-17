import AppKit
import Observation
import SwiftUI

/// Owns only a device window. All controls use the workspace's existing
/// models; opening/closing this window never launches or stops a core.
@MainActor @Observable final class SimulatorDisplayWindow: NSObject, NSWindowDelegate {
    private(set) var isDetached = false
    @ObservationIgnored private(set) var window: NSWindow?
    @ObservationIgnored private var runtimeID: ObjectIdentifier?

    func show(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel,
              recording: SimulatorRecordingModel, guestOperations: SimulatorGuestOperationsModel,
              logStream: SimulatorLogStreamModel, instanceName: String?) {
        let id = ObjectIdentifier(runtime)
        if let window, runtimeID == id { window.makeKeyAndOrderFront(nil); return }
        close()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 940, height: 880),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = instanceName.map { "Azki Simulator · \($0) · 设备页面" } ?? "Azki Simulator · 设备页面"
        window.isReleasedWhenClosed = false
        window.contentMinSize = NSSize(width: 720, height: 640)
        window.contentView = NSHostingView(rootView: SimulatorDetachedDisplay(runtime: runtime, sharing: sharing,
            recording: recording, guestOperations: guestOperations, logStream: logStream, instanceName: instanceName))
        window.delegate = self
        self.window = window; runtimeID = id; isDetached = true
        window.center(); window.makeKeyAndOrderFront(nil)
    }
    func close() {
        guard let window else { isDetached = false; runtimeID = nil; return }
        window.close()
        self.window = nil; runtimeID = nil; isDetached = false
    }
    func windowWillClose(_ notification: Notification) {
        guard let closed = notification.object as? NSWindow, closed === window else { return }
        // Removing the hosting view dismantles Canvas and releases any held
        // contact through the same session input queue before returning inline.
        closed.contentView = nil
        window = nil; runtimeID = nil; isDetached = false
    }
}

@MainActor private struct SimulatorDetachedDisplay: View {
    let runtime: StandaloneRuntimeModel
    let sharing: HostStatusSharingModel
    let recording: SimulatorRecordingModel
    let guestOperations: SimulatorGuestOperationsModel
    let logStream: SimulatorLogStreamModel
    let instanceName: String?
    private var session: SimulatorSessionModel { runtime.runtimeSession }

    var body: some View {
        VSplitView {
            deviceAndControls.frame(minHeight: 300, idealHeight: 620)
            SimulatorLogPanel(runtime: runtime, stream: logStream, instanceName: instanceName)
                .frame(minHeight: 180, idealHeight: 240)
                .accessibilityIdentifier("simulator.detached-log-panel")
        }
        .font(.system(size: 14))
        .foregroundStyle(SimulatorTheme.text)
        .tint(SimulatorTheme.accent)
        .buttonStyle(.bordered)
        .controlSize(.large)
        .background(SimulatorTheme.canvas)
    }

    private var deviceAndControls: some View {
        HSplitView {
            deviceDisplay
                .frame(minWidth: 280, idealWidth: 460, maxWidth: .infinity, maxHeight: .infinity)
            ScrollView {
                SimulatorDeviceControls(runtime: runtime, sharing: sharing, recording: recording,
                    guestOperations: guestOperations, instanceName: instanceName) { EmptyView() }
                    .simulatorSurface()
                    .padding(SimulatorTheme.gap)
            }
            .frame(minWidth: 400, idealWidth: 460, maxWidth: 560, maxHeight: .infinity, alignment: .topLeading)
            .accessibilityIdentifier("simulator.detached-controls")
        }
    }

    private var deviceDisplay: some View {
        VStack(spacing: SimulatorTheme.gap) {
            HStack {
                Text(instanceName ?? "设备画面").fontWeight(.semibold)
                Spacer()
                Text(session.stateLabel).foregroundStyle(SimulatorTheme.secondary)
            }
            if let geometry = session.manifest?.displayGeometry {
                SimulatorNativeCanvas(image: session.image, geometry: geometry, acceptsInput: session.canInput, onInput: session.sendInput)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .accessibilityIdentifier("simulator.detached-canvas")
            } else {
                ContentUnavailableView("设备未运行", systemImage: "applewatch",
                    description: Text("可在右侧选择固件并运行；控制区始终对应本实例。"))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .padding(SimulatorTheme.inset)
    }
}
