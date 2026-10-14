import AppKit
import SwiftUI

/// Native selectable log text. Preserve the first visible character when follow
/// is off; append in place when possible instead of rebuilding every line.
struct SimulatorLogTextView: NSViewRepresentable {
    let text: String
    let followBottom: Bool
    var discardedUTF16: Int = 0
    var epoch: UInt64 = 0
    final class Coordinator {
        var discardedUTF16 = 0
        var epoch: UInt64 = 0
    }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSScrollView {
        let view = NSTextView(frame: .zero)
        view.isEditable = false; view.isSelectable = true; view.isRichText = false
        view.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        view.textColor = .labelColor; view.drawsBackground = false
        view.isVerticallyResizable = true; view.isHorizontallyResizable = true
        view.autoresizingMask = [.width]
        view.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        view.textContainer?.containerSize = view.maxSize
        view.textContainer?.widthTracksTextView = false
        let scroll = NSScrollView()
        scroll.drawsBackground = false; scroll.hasVerticalScroller = true; scroll.hasHorizontalScroller = true
        scroll.documentView = view
        return scroll
    }
    func updateNSView(_ scroll: NSScrollView, context: Context) {
        Self.update(scroll, text: text, followBottom: followBottom,
                    discardedUTF16: discardedUTF16, epoch: epoch, coordinator: context.coordinator)
    }
    static func update(_ scroll: NSScrollView, text: String, followBottom: Bool,
                       discardedUTF16: Int, epoch: UInt64, coordinator: Coordinator) {
        guard let view = scroll.documentView as? NSTextView else { return }
        let removed = epoch == coordinator.epoch ? max(0, discardedUTF16 - coordinator.discardedUTF16) : Int.max
        coordinator.discardedUTF16 = discardedUTF16; coordinator.epoch = epoch
        view.minSize = scroll.contentSize
        if view.string != text {
            let old = view.string
            let origin = scroll.contentView.bounds.origin
            let manager = view.layoutManager, container = view.textContainer
            let glyph = container.flatMap { manager?.glyphIndex(for: origin, in: $0) } ?? 0
            let anchor = manager.flatMap { $0.numberOfGlyphs > 0 ? $0.characterIndexForGlyph(at: min(glyph, $0.numberOfGlyphs - 1)) : nil } ?? 0
            if text.hasPrefix(old) {
                let suffix = String(text.dropFirst(old.count))
                view.textStorage?.append(NSAttributedString(string: suffix, attributes: [.font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular), .foregroundColor: NSColor.labelColor]))
            } else {
                view.string = text
            }
            if !followBottom {
                // Normal appends leave the viewport exactly where the user put
                // it. Rotation/ring trimming clamps a now-unavailable anchor.
                if text.hasPrefix(old) { scroll.contentView.scroll(to: origin) }
                else if let manager, let container, !text.isEmpty {
                    manager.ensureLayout(for: container)
                    let index = min(max(0, anchor - removed), max(0, (text as NSString).length - 1))
                    let range = manager.glyphRange(forCharacterRange: NSRange(location: index, length: 1), actualCharacterRange: nil)
                    let rect = manager.boundingRect(forGlyphRange: range, in: container)
                    scroll.contentView.scroll(to: NSPoint(x: origin.x, y: rect.minY))
                }
                scroll.reflectScrolledClipView(scroll.contentView)
            }
        }
        if let manager = view.layoutManager, let container = view.textContainer {
            manager.ensureLayout(for: container)
            let used = manager.usedRect(for: container)
            view.setFrameSize(NSSize(width: max(scroll.contentSize.width, used.maxX + view.textContainerInset.width * 2),
                                     height: max(scroll.contentSize.height, used.maxY + view.textContainerInset.height * 2)))
        }
        if followBottom { view.scrollToEndOfDocument(nil) }
    }
}
