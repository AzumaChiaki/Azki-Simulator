import CoreGraphics

/// Pure layout math for the device detail view.
///
/// The log pane hosts an NSScrollView whose document view grows with every
/// appended line, and the canvas is itself an NSViewRepresentable. Neither may
/// feed its intrinsic size back into the split: an AppKit-backed VSplitView
/// lets the log content push the canvas up under the window toolbar, then
/// freezes the divider instead of tracking the container as it resizes.
/// Both panes therefore receive concrete heights derived from the measured
/// container, and the canvas takes exactly the remaining space.
nonisolated enum SimulatorDeviceContentLayout {
    static let defaultLogPaneHeight: CGFloat = 320
    static let minimumLogPaneHeight: CGFloat = 120
    static let minimumCanvasHeight: CGFloat = 240
    static let dividerHeight: CGFloat = 8

    /// Largest log pane that still leaves `minimumCanvasHeight` for the canvas.
    static func maximumLogPaneHeight(availableHeight: CGFloat) -> CGFloat {
        guard availableHeight.isFinite else { return defaultLogPaneHeight }
        return max(minimumLogPaneHeight, availableHeight - dividerHeight - minimumCanvasHeight)
    }

    /// Concrete log pane height for the measured container and the user's
    /// preferred (dragged) height. Always finite and never below the minimum.
    static func logPaneHeight(availableHeight: CGFloat, preferred: CGFloat) -> CGFloat {
        let wanted = preferred.isFinite ? max(minimumLogPaneHeight, preferred) : defaultLogPaneHeight
        return min(wanted, maximumLogPaneHeight(availableHeight: availableHeight))
    }

    /// Height left for the canvas once the divider and log pane are placed.
    static func canvasHeight(availableHeight: CGFloat, logPaneHeight: CGFloat) -> CGFloat {
        guard availableHeight.isFinite, logPaneHeight.isFinite else { return 0 }
        return max(0, availableHeight - dividerHeight - logPaneHeight)
    }
}
