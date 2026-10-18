import AppKit
import SwiftUI

struct SimulatorNativeCanvas: NSViewRepresentable {
    let image: CGImage?
    let geometry: SimulatorDisplayGeometry
    let acceptsInput: Bool
    let onInput: (SimulatorInputEvent) -> Void
    /// Physical watch-button clicks from the control area: "crown" (middle
    /// click) and "lower" (right click). Nil or unsupported hardware ignores
    /// those buttons entirely.
    var onDeviceButton: ((String) -> Void)? = nil

    init(image: CGImage?, geometry: SimulatorDisplayGeometry = .pixels212x520,
         acceptsInput: Bool, onInput: @escaping (SimulatorInputEvent) -> Void,
         onDeviceButton: ((String) -> Void)? = nil) {
        self.image = image; self.geometry = geometry; self.acceptsInput = acceptsInput; self.onInput = onInput
        self.onDeviceButton = onDeviceButton
    }

    func makeNSView(context: Context) -> SimulatorCanvasNSView { SimulatorCanvasNSView(frame: .zero) }
    func updateNSView(_ view: SimulatorCanvasNSView, context: Context) {
        view.onInput = onInput
        view.onDeviceButton = onDeviceButton
        view.displayGeometry = geometry
        view.acceptsInput = acceptsInput
        view.image = image
    }
    static func dismantleNSView(_ view: SimulatorCanvasNSView, coordinator: ()) {
        view.cancelContact()
        view.removeWindowObservers()
        view.onInput = nil
        view.onDeviceButton = nil
    }
}

/// Draws the shared RGB565-decoded frame through AppKit. No browser, subprocess
/// window, UIKit bridge or executable is part of the rendering surface.
@MainActor final class SimulatorCanvasNSView: NSView {
    var onInput: ((SimulatorInputEvent) -> Void)?
    var onDeviceButton: ((String) -> Void)?
    var acceptsInput = false { didSet { if !acceptsInput { cancelContact() } } }
    var displayGeometry: SimulatorDisplayGeometry = .pixels212x520 {
        didSet {
            guard displayGeometry != oldValue else { return }
            cancelContact()
            updateBitmap()
        }
    }
    var image: CGImage? {
        didSet {
            guard image !== oldValue else { return }
            updateBitmap()
        }
    }
    // Counts host work only, never display/vsync FPS. Useful to detect redundant
    // SwiftUI updates without adding an observable property or per-frame logs.
    private(set) var bitmapUpdateCount: UInt64 = 0
    private(set) var drawCount: UInt64 = 0
    private var bitmap: NSImage?
    private var contact: SimulatorInputEvent?
    private var pointerTracking: NSTrackingArea?
    private var windowObservers: [NSObjectProtocol] = []
    private var deviceSize: CGSize { displayGeometry.size }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }
    override var acceptsFirstResponder: Bool { acceptsInput }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setAccessibilityElement(true)
        setAccessibilityLabel("小米手环模拟器画面")
        setAccessibilityHelp("使用鼠标点击或拖动操作设备画面；Escape 释放当前接触")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    private func updateBitmap() {
        bitmapUpdateCount &+= 1
        if let image, image.width == displayGeometry.width, image.height == displayGeometry.height {
            bitmap = NSImage(cgImage: image, size: deviceSize)
        } else { bitmap = nil }
        needsDisplay = true
        setAccessibilityValue(bitmap == nil ? "尚无匹配的设备画面" : "\(displayGeometry.width) × \(displayGeometry.height) 像素")
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let bitmap else { return }
        drawCount &+= 1
        let rect = SimulatorCanvasGeometry.contentRect(in: bounds, pixelSize: deviceSize)
        NSColor.black.setFill()
        NSBezierPath(rect: rect).fill()
        NSGraphicsContext.current?.imageInterpolation = .none
        bitmap.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        guard acceptsInput, let event else { return false }
        return mapped(convert(event.locationInWindow, from: nil), down: true) != nil
    }
    override func mouseDown(with event: NSEvent) {
        guard beginContact(at: convert(event.locationInWindow, from: nil)) else { return }
        window?.makeFirstResponder(self)
    }
    override func mouseDragged(with event: NSEvent) { moveContact(to: convert(event.locationInWindow, from: nil)) }
    override func mouseUp(with event: NSEvent) { endContact(at: convert(event.locationInWindow, from: nil)) }
    override func mouseExited(with event: NSEvent) { cancelContact() }
    override func cancelOperation(_ sender: Any?) { cancelContact() }

    /// Middle click = the optical crown pressed as a button (single click).
    override func otherMouseDown(with event: NSEvent) {
        guard acceptsInput, event.buttonNumber == 2, let onDeviceButton else {
            super.otherMouseDown(with: event); return
        }
        onDeviceButton("crown")
    }

    /// Right click = the watch's lower hardware key (single click).
    override func rightMouseDown(with event: NSEvent) {
        guard acceptsInput, let onDeviceButton else {
            super.rightMouseDown(with: event); return
        }
        onDeviceButton("lower")
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let pointerTracking { removeTrackingArea(pointerTracking) }
        let area = NSTrackingArea(rect: .zero,
            options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect, .enabledDuringMouseDrag], owner: self, userInfo: nil)
        addTrackingArea(area)
        pointerTracking = area
    }

    override func setFrameSize(_ newSize: NSSize) {
        if frame.size != newSize { cancelContact() }
        super.setFrameSize(newSize)
        needsDisplay = true
    }
    override func setBoundsSize(_ newSize: NSSize) {
        if bounds.size != newSize { cancelContact() }
        super.setBoundsSize(newSize)
        needsDisplay = true
    }
    override func viewWillMove(toWindow newWindow: NSWindow?) {
        cancelContact()
        removeWindowObservers()
        super.viewWillMove(toWindow: newWindow)
        guard let newWindow else { return }
        for name in [NSWindow.didResignKeyNotification, NSWindow.didMiniaturizeNotification, NSWindow.willCloseNotification] {
            windowObservers.append(NotificationCenter.default.addObserver(forName: name, object: newWindow, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.cancelContact() }
            })
        }
    }

    func removeWindowObservers() {
        for observer in windowObservers { NotificationCenter.default.removeObserver(observer) }
        windowObservers.removeAll()
    }

    @discardableResult func beginContact(at point: CGPoint) -> Bool {
        guard acceptsInput, contact == nil, let input = mapped(point, down: true) else { return false }
        contact = input
        onInput?(input)
        return true
    }
    func moveContact(to point: CGPoint) {
        guard contact != nil else { return }
        guard acceptsInput, let input = mapped(point, down: true) else { cancelContact(); return }
        guard input != contact else { return }
        contact = input
        onInput?(input)
    }
    func endContact(at point: CGPoint) {
        guard contact != nil else { return }
        if let input = mapped(point, down: false) { contact = input }
        cancelContact()
    }
    func cancelContact() {
        if let contact { onInput?(SimulatorInputEvent(x: contact.x, y: contact.y, down: false)) }
        contact = nil
    }
    private func mapped(_ point: CGPoint, down: Bool) -> SimulatorInputEvent? {
        SimulatorCanvasGeometry.input(at: point, bounds: bounds, pixelSize: deviceSize, down: down)
    }
}
