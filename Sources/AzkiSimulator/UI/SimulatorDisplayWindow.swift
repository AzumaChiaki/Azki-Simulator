import AppKit
import Observation
import SwiftUI

/// Owns only a display window. The passed session is always the workspace's
/// existing object; opening/closing this window never launches or stops a core.
@MainActor @Observable final class SimulatorDisplayWindow: NSObject, NSWindowDelegate {
    private(set) var isDetached = false
    @ObservationIgnored private(set) var window: NSWindow?
    @ObservationIgnored private var sessionID: UUID?

    func show(session: SimulatorSessionModel) {
        guard let id = session.manifest?.id else { return }
        if let window, sessionID == id { window.makeKeyAndOrderFront(nil); return }
        close()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 390, height: 680),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Azki Simulator · 设备画面"
        window.isReleasedWhenClosed = false
        window.contentMinSize = NSSize(width: 280, height: 420)
        window.contentView = NSHostingView(rootView: SimulatorDetachedDisplay(session: session))
        window.delegate = self
        self.window = window; sessionID = id; isDetached = true
        window.center(); window.makeKeyAndOrderFront(nil)
    }
    func close() {
        guard let window else { isDetached = false; sessionID = nil; return }
        window.close()
        self.window = nil; sessionID = nil; isDetached = false
    }
    func reconcile(sessionID: UUID?) {
        if self.sessionID != sessionID { close() }
    }
    func windowWillClose(_ notification: Notification) {
        guard let closed = notification.object as? NSWindow, closed === window else { return }
        // Removing the hosting view dismantles Canvas and releases any held
        // contact through the same session input queue before returning inline.
        closed.contentView = nil
        window = nil; sessionID = nil; isDetached = false
    }
}

@MainActor private struct SimulatorDetachedDisplay: View {
    @Bindable var session: SimulatorSessionModel
    var body: some View {
        VStack(spacing: SimulatorTheme.gap) {
            HStack { Text("设备画面").fontWeight(.semibold); Spacer(); Text(session.stateLabel).foregroundStyle(SimulatorTheme.secondary) }
            if let geometry = session.manifest?.displayGeometry {
                SimulatorNativeCanvas(image: session.image, geometry: geometry, acceptsInput: session.canInput, onInput: session.sendInput)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .accessibilityIdentifier("simulator.detached-canvas")
            } else { Text("会话已断开").foregroundStyle(SimulatorTheme.secondary) }
        }
        .padding(SimulatorTheme.inset)
        .foregroundStyle(SimulatorTheme.text)
        .background(SimulatorTheme.canvas)
    }
}
