import AppKit
import SwiftUI

/// Portable Azki Studio design values, kept in one standalone UI boundary.
/// Provenance: AzkiDeck/Design/Theme.swift Palette.Classic (colors),
/// ThemeConfiguration.swift ThemeStyleParameters (12 gap / 16 inset / 46 action),
/// and ThemeLayout.swift (24 card radius / 34 page title). Native macOS controls
/// retain intrinsic sizes; this app has no access to Studio's preference store.
enum SimulatorTheme {
    static let gap: CGFloat = 12
    static let inset: CGFloat = 16
    static let radius: CGFloat = 24
    static let titleSize: CGFloat = 34
    static let pageInset: CGFloat = 24
    static let minimumSectionWidth: CGFloat = 380

    static let canvas = adaptive(light: 0xFBF8F2, dark: 0x000000)
    static let text = adaptive(light: 0x23201B, dark: 0xE8E8E8)
    static let secondary = adaptive(light: 0x6E675C, dark: 0xA6A6A6)
    static let surface = adaptive(light: 0xFFFFFF, dark: 0x181818)
    static let accent = adaptive(light: 0x8F5552, dark: 0xD99B97)

    private static func adaptive(light: UInt32, dark: UInt32) -> Color {
        Color(nsColor: NSColor(name: nil) { appearance in
            let value = appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua ? dark : light
            return NSColor(srgbRed: Double((value >> 16) & 255) / 255,
                green: Double((value >> 8) & 255) / 255, blue: Double(value & 255) / 255, alpha: 1)
        })
    }
}

private struct SimulatorSurface: ViewModifier {
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    func body(content: Content) -> some View {
        if reduceTransparency {
            content.background(SimulatorTheme.surface, in: RoundedRectangle(cornerRadius: SimulatorTheme.radius))
        } else {
            content.background(.regularMaterial, in: RoundedRectangle(cornerRadius: SimulatorTheme.radius))
        }
    }
}

extension View {
    func simulatorSurface() -> some View { modifier(SimulatorSurface()) }
    func simulatorActionLabel() -> some View {
        font(.system(size: 14, weight: .medium))
            .lineLimit(1).fixedSize(horizontal: true, vertical: true)
    }
}

struct SimulatorSection<Content: View>: View {
    let title: String
    let icon: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: SimulatorTheme.gap) {
            Label(title, systemImage: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(SimulatorTheme.text)
                .accessibilityAddTraits(.isHeader)
            content
        }
        .padding(SimulatorTheme.inset)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct SimulatorSectionDivider: View {
    var body: some View { Divider().padding(.horizontal, SimulatorTheme.inset) }
}

/// Port of Studio's ThemeActionFlow principle: move whole native actions to a
/// new row rather than compressing type or wrapping button labels.
struct SimulatorActionFlow: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        let width = proposal.width ?? sizes.reduce(0) { $0 + $1.width + SimulatorTheme.gap }
        return CGSize(width: width, height: positions(sizes, width: width).height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        for (index, origin) in positions(sizes, width: bounds.width).origins.enumerated() {
            subviews[index].place(at: CGPoint(x: bounds.minX + origin.x, y: bounds.minY + origin.y),
                anchor: .topLeading, proposal: ProposedViewSize(sizes[index]))
        }
    }

    private func positions(_ sizes: [CGSize], width: CGFloat) -> (origins: [CGPoint], height: CGFloat) {
        var x: CGFloat = 0, y: CGFloat = 0, rowHeight: CGFloat = 0
        var origins: [CGPoint] = []
        for size in sizes {
            if x > 0, x + size.width > width {
                x = 0; y += rowHeight + SimulatorTheme.gap; rowHeight = 0
            }
            origins.append(CGPoint(x: x, y: y))
            x += size.width + SimulatorTheme.gap
            rowHeight = max(rowHeight, size.height)
        }
        return (origins, y + rowHeight)
    }
}
