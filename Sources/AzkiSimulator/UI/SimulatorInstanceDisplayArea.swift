import AppKit
import SwiftUI

@MainActor struct SimulatorInstanceDisplayArea: View {
    let library: SimulatorInstanceLibrary
    @Binding var selection: SimulatorDisplaySelection
    let workspaceID: UUID

    private var instances: [SimulatorInstanceContext] { library.instances }
    private var availableIDs: Set<UUID> { Set(instances.map(\.id)) }
    private var displayableIDs: Set<UUID> {
        Set(instances.filter { $0.runtime.runtimeSession.isRunning || $0.runtime.runtimeSession.image != nil }.map(\.id))
    }

    var body: some View {
        SimulatorInstanceDropContainer(workspaceID: workspaceID,
            displayableIDs: { displayableIDs },
            pin: { selection.pin($0, availableIDs: availableIDs) }) {
                displayContent
            }
            .accessibilityIdentifier("simulator.display-area")
    }

    private var displayContent: some View {
        GeometryReader { viewport in
            let displayedIDs = selection.displayedIDs(among: displayableIDs)
            let layout = SimulatorDisplayGridLayout(availableWidth: viewport.size.width, itemCount: displayedIDs.count)
            ScrollView(.vertical) {
                LazyVGrid(columns: Array(repeating: GridItem(.fixed(layout.columnWidth), spacing: layout.spacing, alignment: .top), count: layout.columns),
                          alignment: .center, spacing: 28) {
                    ForEach(displayedIDs, id: \.self) { id in
                        if let target = selection.inputTarget(for: id, availableIDs: availableIDs),
                           let instance = instances.first(where: { $0.id == target }) {
                            SimulatorInstanceDisplayCard(instance: instance, layout: layout) {
                                let removed = selection.close(id, availableIDs: availableIDs)
                                NSLog("[AzkiDisplayDrag] close instance=%@ removed=%d", id.uuidString, removed ? 1 : 0)
                            }
                        }
                    }
                    if displayedIDs.isEmpty {
                        ContentUnavailableView(displayableIDs.isEmpty ? "启动设备以显示画面" : "选择或拖入设备", systemImage: "applewatch")
                            .frame(width: layout.columnWidth, height: 420)
                    }
                }
                .padding(.horizontal, layout.horizontalInset)
                .padding(.vertical, 16)
                .frame(width: viewport.size.width, alignment: .top)
            }.frame(width: viewport.size.width, height: viewport.size.height)
        }
        .clipped()
        .background(SimulatorTheme.canvas)
    }
}

/// The card receives its instance directly. Sidebar selection cannot redirect
/// this canvas' touch events into any other instance's session.
@MainActor private struct SimulatorInstanceDisplayCard: View {
    let instance: SimulatorInstanceContext
    let layout: SimulatorDisplayGridLayout
    let close: () -> Void
    private var session: SimulatorSessionModel { instance.runtime.runtimeSession }

    var body: some View {
        VStack(alignment: .center, spacing: 12) {
            Text(instance.record.name).font(.headline).lineLimit(2)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .contextMenu {
                    Button("收起画面", action: close)
                        .accessibilityIdentifier("simulator.close-display.\(instance.id.uuidString)")
                }
                .help("右键点击名称可收起画面，设备继续运行")
            if let geometry = session.manifest?.displayGeometry {
                let size = layout.canvasSize(geometry.size)
                SimulatorNativeCanvas(image: session.image, geometry: geometry,
                                      acceptsInput: session.canInput, onInput: session.sendInput)
                    .frame(width: size.width, height: size.height)
                    .accessibilityIdentifier("simulator.canvas.\(instance.id.uuidString)")
            }
        }
        .frame(width: layout.columnWidth, alignment: .top)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("simulator.display.\(instance.id.uuidString)")
    }
}

/// The hosting surface exposes its real coordinate bounds to local dragging.
/// No overlay intercepts canvas touch and no system drag session is started.
@MainActor private struct SimulatorInstanceDropContainer<Content: View>: NSViewRepresentable {
    let workspaceID: UUID
    let displayableIDs: () -> Set<UUID>
    let pin: (UUID) -> Bool
    @ViewBuilder let content: () -> Content

    func makeNSView(context: Context) -> SimulatorInstanceDropView<Content> {
        let view = SimulatorInstanceDropView(rootView: content())
        configure(view)
        return view
    }
    func updateNSView(_ view: SimulatorInstanceDropView<Content>, context: Context) {
        configure(view)
        view.rootView = content()
    }
    private func configure(_ view: SimulatorInstanceDropView<Content>) {
        view.workspaceID = workspaceID
        view.displayableIDs = displayableIDs
        view.pin = pin
    }
}

@MainActor private final class SimulatorInstanceDropView<Content: View>: NSHostingView<Content>, SimulatorInstanceInternalDropTarget {
    var workspaceID: UUID?
    var displayableIDs: (() -> Set<UUID>)?
    var pin: ((UUID) -> Bool)?
    private var highlighted = false
    var targetWindow: NSWindow? { window }

    required init(rootView: Content) {
        super.init(rootView: rootView)
        sizingOptions = []
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private func instanceID(_ gesture: SimulatorInstanceDragGesture, pointInWindow: NSPoint) -> UUID? {
        guard let window, let workspaceID, !isHiddenOrHasHiddenAncestor else { return nil }
        return gesture.targetInstanceID(workspaceID: workspaceID, windowNumber: window.windowNumber,
            bounds: bounds, locationInTarget: convert(pointInWindow, from: nil), displayableIDs: displayableIDs?() ?? [])
    }
    func accepts(_ gesture: SimulatorInstanceDragGesture, pointInWindow: NSPoint) -> Bool {
        instanceID(gesture, pointInWindow: pointInWindow) != nil
    }
    func receive(_ gesture: SimulatorInstanceDragGesture, pointInWindow: NSPoint) -> Bool {
        guard let id = instanceID(gesture, pointInWindow: pointInWindow), let pin else { return false }
        let added = pin(id)
        NSLog("[AzkiDisplayDrag] internal.drop instance=%@ added=%d eventWindow=%@ targetPoint=%@ targetBounds=%@", id.uuidString,
              added ? 1 : 0, NSStringFromPoint(pointInWindow), NSStringFromPoint(convert(pointInWindow, from: nil)), NSStringFromRect(bounds))
        return true
    }
    func setHighlighted(_ value: Bool) {
        guard value != highlighted else { return }
        highlighted = value
        wantsLayer = true
        layer?.borderColor = NSColor.controlAccentColor.withAlphaComponent(0.65).cgColor
        layer?.borderWidth = value ? 2 : 0
        NSLog("[AzkiDisplayDrag] internal.target highlighted=%d bounds=%@ windowRect=%@", value ? 1 : 0,
              NSStringFromRect(bounds), NSStringFromRect(convert(bounds, to: nil)))
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        SimulatorInstanceInternalDragRegistry.register(self)
        logWindow("attached")
    }
    override func viewWillStartLiveResize() {
        super.viewWillStartLiveResize()
        logWindow("begin")
    }
    override func viewDidEndLiveResize() {
        super.viewDidEndLiveResize()
        logWindow("end")
    }
    private func logWindow(_ phase: String) {
        guard let window else { return }
        NSLog("[AzkiDisplayResize] phase=%@ window=%@ min=%@ contentMin=%@ display=%@ resizable=%d hostFrame=%@ parentBounds=%@", phase,
              NSStringFromRect(window.frame), NSStringFromSize(window.minSize), NSStringFromSize(window.contentMinSize),
              NSStringFromRect(bounds), window.styleMask.contains(.resizable) ? 1 : 0, NSStringFromRect(frame),
              NSStringFromRect(superview?.bounds ?? .zero))
    }
}

@MainActor protocol SimulatorInstanceInternalDropTarget: AnyObject {
    var targetWindow: NSWindow? { get }
    func accepts(_ gesture: SimulatorInstanceDragGesture, pointInWindow: NSPoint) -> Bool
    func receive(_ gesture: SimulatorInstanceDragGesture, pointInWindow: NSPoint) -> Bool
    func setHighlighted(_ highlighted: Bool)
}

/// Weak references cannot keep a closed window or its instance sessions alive.
/// Coordinates are converted by the target after checking the exact NSWindow.
@MainActor enum SimulatorInstanceInternalDragRegistry {
    private struct WeakTarget { weak var value: (any SimulatorInstanceInternalDropTarget)? }
    private static var targets: [WeakTarget] = []

    static func register(_ target: any SimulatorInstanceInternalDropTarget) {
        targets.removeAll { $0.value == nil || $0.value === target }
        if target.targetWindow != nil { targets.append(WeakTarget(value: target)) }
    }
    static func target(for gesture: SimulatorInstanceDragGesture, window: NSWindow,
                       point: NSPoint) -> (any SimulatorInstanceInternalDropTarget)? {
        targets.removeAll { $0.value == nil }
        return targets.reversed().compactMap(\.value).first {
            $0.targetWindow === window && $0.accepts(gesture, pointInWindow: point)
        }
    }
    static func updateHighlight(for gesture: SimulatorInstanceDragGesture, window: NSWindow, point: NSPoint) {
        let selected = target(for: gesture, window: window, point: point)
        for target in targets.compactMap(\.value) { target.setHighlighted(target === selected) }
    }
    static func clearHighlights() {
        for target in targets.compactMap(\.value) { target.setHighlighted(false) }
    }
}
