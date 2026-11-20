import AppKit
import SwiftUI

// MARK: - Gemini Palette (Clean Surfaces + Restrained Smoky Neon Accents)

enum GeminiPalette {
    // Signature Gemini hues — used with restraint for smoky ambient layers and crisp solid accents.
    static let blue = Color(red: 0.20, green: 0.48, blue: 0.96)       // Clean Gemini Blue
    static let blueDark = Color(red: 0.34, green: 0.58, blue: 0.99)   // Luminous Blue for Dark Mode
    static let indigo = Color(red: 0.44, green: 0.48, blue: 0.98)     // Periwinkle Indigo
    static let violet = Color(red: 0.61, green: 0.45, blue: 0.84)     // Soft Violet Mist
    static let orchid = Color(red: 0.86, green: 0.39, blue: 0.48)     // Restrained Rose-Orchid
    static let cyan = Color(red: 0.20, green: 0.72, blue: 0.96)       // Ethereal Cyan
    static let emerald = Color(red: 0.18, green: 0.78, blue: 0.56)    // Live Running Green
    static let amber = Color(red: 0.94, green: 0.62, blue: 0.16)      // Subtle Warning Amber
    static let coral = Color(red: 0.90, green: 0.29, blue: 0.35)      // Recording / Destructive

    static func accent(for scheme: ColorScheme) -> Color {
        scheme == .dark ? blueDark : blue
    }

    static func stageBackground(for scheme: ColorScheme) -> Color {
        scheme == .dark
            ? Color(red: 0.07, green: 0.08, blue: 0.11)
            : Color(red: 0.965, green: 0.973, blue: 0.99)
    }

    static func sidebarBackground(for scheme: ColorScheme) -> Color {
        scheme == .dark
            ? Color(red: 0.085, green: 0.095, blue: 0.125)
            : Color(red: 0.955, green: 0.965, blue: 0.985)
    }

    static func inspectorBackground(for scheme: ColorScheme) -> Color {
        scheme == .dark
            ? Color(red: 0.08, green: 0.09, blue: 0.12)
            : Color(red: 0.965, green: 0.972, blue: 0.988)
    }

    static func cardFill(for scheme: ColorScheme, elevated: Bool = false) -> Color {
        if scheme == .dark {
            return elevated
                ? Color(red: 0.13, green: 0.15, blue: 0.20)
                : Color(red: 0.105, green: 0.12, blue: 0.16)
        } else {
            return elevated
                ? Color.white
                : Color.white.opacity(0.86)
        }
    }

    static func selectedRowFill(for scheme: ColorScheme) -> Color {
        scheme == .dark
            ? Color(red: 0.15, green: 0.20, blue: 0.30)
            : Color(red: 0.895, green: 0.935, blue: 1.0)
    }

    static func hoverRowFill(for scheme: ColorScheme) -> Color {
        scheme == .dark
            ? Color.white.opacity(0.055)
            : Color.black.opacity(0.04)
    }

    static func hairlineBorder(for scheme: ColorScheme) -> Color {
        scheme == .dark ? Color.white.opacity(0.09) : Color.black.opacity(0.085)
    }

    static func pillFill(for scheme: ColorScheme, hovered: Bool) -> Color {
        if scheme == .dark {
            return Color.white.opacity(hovered ? 0.12 : 0.07)
        } else {
            return Color.black.opacity(hovered ? 0.075 : 0.042)
        }
    }
}

// MARK: - GPU-Composited Restrained Smoke & Neon Mist Backdrop (Zero-CPU-Loop)

enum GeminiAtmosphereIntensity {
    case ambient
    case stage
    case hero

    func opacityScale(for scheme: ColorScheme, active: Bool) -> Double {
        let base: Double = switch self {
        case .ambient: scheme == .dark ? 0.45 : 0.28
        case .stage: scheme == .dark ? 0.78 : 0.44
        case .hero: scheme == .dark ? 0.92 : 0.54
        }
        return active ? base * 1.18 : base
    }
}

/// Pure GPU CoreAnimation atmospheric smoke layer. Uses pre-softened radial
/// gradients (no expensive blur passes and no per-frame TimelineView CPU redraws)
/// so transitions and live device rendering stay locked at full display refresh rate.
struct GeminiAtmosphereBackdrop: View {
    var intensity: GeminiAtmosphereIntensity = .stage
    var isActive: Bool = false

    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var drift = false

    var body: some View {
        let scale = intensity.opacityScale(for: colorScheme, active: isActive)
        GeometryReader { proxy in
            let w = max(1, proxy.size.width)
            let h = max(1, proxy.size.height)
            let d = max(240, min(w, h))

            ZStack {
                GeminiPalette.stageBackground(for: colorScheme)

                // Plume 1: Cool Gemini Blue smoke upper-left
                smokePlume(color: GeminiPalette.blue, opacity: 0.16 * scale, diameter: d * 1.15)
                    .position(x: w * 0.24, y: h * 0.26)
                    .offset(x: drift ? 28 : -18, y: drift ? -16 : 20)

                // Plume 2: Soft Violet mist lower-right
                smokePlume(color: GeminiPalette.violet, opacity: 0.14 * scale, diameter: d * 1.2)
                    .position(x: w * 0.78, y: h * 0.72)
                    .offset(x: drift ? -26 : 18, y: drift ? 18 : -20)

                // Plume 3: Ethereal Cyan/Indigo center-top wisp (enriches subtly when running)
                smokePlume(
                    color: isActive ? GeminiPalette.cyan : GeminiPalette.indigo,
                    opacity: (isActive ? 0.14 : 0.08) * scale,
                    diameter: d * 0.95
                )
                .position(x: w * 0.54, y: h * 0.34)
                .offset(x: drift ? -20 : 24, y: drift ? 14 : -14)

                // Plume 4: Whisper-soft Rose-Orchid warm neon mist lower-left
                smokePlume(color: GeminiPalette.orchid, opacity: 0.08 * scale, diameter: d * 0.85)
                    .position(x: w * 0.28, y: h * 0.80)
                    .offset(x: drift ? 18 : -16, y: drift ? -12 : 12)
            }
            .drawingGroup(opaque: true)
        }
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 16).repeatForever(autoreverses: true)) {
                drift = true
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func smokePlume(color: Color, opacity: Double, diameter: CGFloat) -> some View {
        Circle()
            .fill(
                RadialGradient(
                    stops: [
                        .init(color: color.opacity(opacity), location: 0.0),
                        .init(color: color.opacity(opacity * 0.45), location: 0.42),
                        .init(color: color.opacity(opacity * 0.10), location: 0.74),
                        .init(color: color.opacity(0.0), location: 1.0)
                    ],
                    center: .center,
                    startRadius: 0,
                    endRadius: diameter * 0.5
                )
            )
            .frame(width: diameter, height: diameter)
    }
}

/// Isolated interactive pointer-following smoke bloom. Kept in its own leaf view
/// so mouse motion never triggers re-evaluation of the device canvas or HUD.
struct GeminiInteractiveMistOverlay: View {
    var isActive: Bool = false
    @Environment(\.colorScheme) private var colorScheme
    @State private var pointer: CGPoint?

    var body: some View {
        GeometryReader { _ in
            ZStack {
                Color.clear
                if let pointer {
                    let opacity = colorScheme == .dark ? (isActive ? 0.13 : 0.09) : (isActive ? 0.075 : 0.05)
                    Circle()
                        .fill(
                            RadialGradient(
                                stops: [
                                    .init(color: (isActive ? GeminiPalette.cyan : GeminiPalette.indigo).opacity(opacity), location: 0.0),
                                    .init(color: GeminiPalette.violet.opacity(opacity * 0.45), location: 0.48),
                                    .init(color: Color.clear, location: 1.0)
                                ],
                                center: .center,
                                startRadius: 0,
                                endRadius: 170
                            )
                        )
                        .frame(width: 340, height: 340)
                        .position(pointer)
                        .allowsHitTesting(false)
                }
            }
            .contentShape(Rectangle())
            .onContinuousHover { phase in
                switch phase {
                case .active(let location):
                    pointer = location
                case .ended:
                    pointer = nil
                }
            }
        }
        .accessibilityHidden(true)
    }
}

// MARK: - Clean Solid Gemini Button Style (No Gradients, Zero-Lag Transitions)

enum GeminiButtonTone {
    case primary
    case secondary
    case subtle
    case active
    case recording
    case destructive
}

struct GeminiNeonButtonStyle: ButtonStyle {
    var tone: GeminiButtonTone = .secondary
    var compact: Bool = false

    func makeBody(configuration: Configuration) -> some View {
        GeminiNeonButtonBody(configuration: configuration, tone: tone, compact: compact)
    }
}

private struct GeminiNeonButtonBody: View {
    let configuration: ButtonStyle.Configuration
    let tone: GeminiButtonTone
    let compact: Bool

    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.isEnabled) private var isEnabled
    @State private var isHovered = false

    var body: some View {
        configuration.label
            .font(.system(size: compact ? 11.5 : 12.5, weight: fontWeight))
            .foregroundStyle(foregroundColor)
            .padding(.horizontal, compact ? 10 : 14)
            .padding(.vertical, compact ? 5 : 7)
            .background(Capsule().fill(backgroundColor))
            .overlay(Capsule().strokeBorder(borderColor, lineWidth: 1))
            .shadow(color: hoverGlowColor, radius: 6, x: 0, y: 1)
            .scaleEffect(configuration.isPressed && isEnabled ? 0.98 : 1.0)
            .opacity(isEnabled ? 1.0 : 0.42)
            .animation(.easeOut(duration: 0.14), value: isHovered)
            .animation(.easeOut(duration: 0.10), value: configuration.isPressed)
            .onHover { isHovered = $0 }
    }

    private var fontWeight: Font.Weight {
        switch tone {
        case .primary, .active, .recording: .semibold
        default: .medium
        }
    }

    private var foregroundColor: Color {
        switch tone {
        case .primary:
            return .white
        case .active:
            return GeminiPalette.accent(for: colorScheme)
        case .recording:
            return colorScheme == .dark ? Color(red: 1.0, green: 0.72, blue: 0.75) : GeminiPalette.coral
        case .destructive:
            return colorScheme == .dark ? Color(red: 0.98, green: 0.56, blue: 0.60) : Color(red: 0.80, green: 0.18, blue: 0.24)
        case .secondary, .subtle:
            return .primary
        }
    }

    private var backgroundColor: Color {
        switch tone {
        case .primary:
            let base = GeminiPalette.accent(for: colorScheme)
            if configuration.isPressed { return base.opacity(0.86) }
            return isHovered ? base.opacity(0.92) : base
        case .active:
            let base = GeminiPalette.accent(for: colorScheme)
            return base.opacity(colorScheme == .dark ? (isHovered ? 0.24 : 0.17) : (isHovered ? 0.15 : 0.10))
        case .recording:
            return GeminiPalette.coral.opacity(colorScheme == .dark ? (isHovered ? 0.28 : 0.20) : (isHovered ? 0.16 : 0.11))
        case .destructive:
            return GeminiPalette.coral.opacity(isHovered ? 0.16 : 0.08)
        case .secondary:
            return GeminiPalette.pillFill(for: colorScheme, hovered: isHovered)
        case .subtle:
            return isHovered ? GeminiPalette.pillFill(for: colorScheme, hovered: true) : .clear
        }
    }

    private var borderColor: Color {
        switch tone {
        case .primary:
            return Color.white.opacity(isHovered ? 0.24 : 0.10)
        case .active:
            return GeminiPalette.accent(for: colorScheme).opacity(isHovered ? 0.60 : 0.38)
        case .recording:
            return GeminiPalette.coral.opacity(isHovered ? 0.65 : 0.42)
        case .destructive:
            return GeminiPalette.coral.opacity(isHovered ? 0.50 : 0.22)
        case .secondary:
            if isHovered && isEnabled {
                return GeminiPalette.accent(for: colorScheme).opacity(0.45)
            }
            return GeminiPalette.hairlineBorder(for: colorScheme)
        case .subtle:
            return isHovered && isEnabled ? GeminiPalette.hairlineBorder(for: colorScheme) : .clear
        }
    }

    private var hoverGlowColor: Color {
        guard isEnabled, isHovered else { return .clear }
        switch tone {
        case .primary:
            return GeminiPalette.blue.opacity(colorScheme == .dark ? 0.30 : 0.22)
        case .active:
            return GeminiPalette.cyan.opacity(0.18)
        case .recording, .destructive:
            return GeminiPalette.coral.opacity(0.20)
        case .secondary:
            return GeminiPalette.indigo.opacity(colorScheme == .dark ? 0.18 : 0.10)
        case .subtle:
            return .clear
        }
    }
}

// MARK: - Clean Surface Card (No Rainbow Border, Crisp Hover Rim)

struct GeminiGlassCard<Content: View>: View {
    var cornerRadius: CGFloat = 14
    var isSelected: Bool = false
    var isActive: Bool = false
    var hoverGlow: Bool = true
    var padding: CGFloat = 12
    @ViewBuilder var content: () -> Content

    @Environment(\.colorScheme) private var colorScheme
    @State private var isHovered = false

    var body: some View {
        content()
            .padding(padding)
            .background(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(GeminiPalette.cardFill(for: colorScheme, elevated: isSelected || isHovered))
            )
            .overlay(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .strokeBorder(borderColor, lineWidth: 1)
            )
            .shadow(
                color: Color.black.opacity(colorScheme == .dark ? 0.22 : 0.05),
                radius: 6,
                x: 0,
                y: 2
            )
            .animation(.easeOut(duration: 0.15), value: isHovered)
            .onHover { if hoverGlow { isHovered = $0 } }
    }

    private var borderColor: Color {
        if isSelected {
            return GeminiPalette.accent(for: colorScheme).opacity(0.55)
        } else if isHovered && hoverGlow {
            return GeminiPalette.accent(for: colorScheme).opacity(0.32)
        } else if isActive {
            return GeminiPalette.emerald.opacity(0.36)
        }
        return GeminiPalette.hairlineBorder(for: colorScheme)
    }
}

// MARK: - Gemini Status Pill (Subtle Live Dot)

enum GeminiStatusTone {
    case running
    case connecting
    case idle
    case warning
    case recording

    var dotColor: Color {
        switch self {
        case .running: GeminiPalette.emerald
        case .connecting: GeminiPalette.cyan
        case .idle: Color.secondary.opacity(0.55)
        case .warning: GeminiPalette.amber
        case .recording: GeminiPalette.coral
        }
    }

    var isLive: Bool {
        switch self {
        case .running, .connecting, .recording: true
        case .idle, .warning: false
        }
    }
}

struct GeminiStatusPill: View {
    let text: String
    let tone: GeminiStatusTone
    var compact: Bool = false

    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        HStack(spacing: compact ? 5 : 6) {
            Circle()
                .fill(tone.dotColor)
                .frame(width: compact ? 6 : 7, height: compact ? 6 : 7)
                .shadow(color: tone.isLive ? tone.dotColor.opacity(0.60) : .clear, radius: 4)
            Text(text)
                .font(.system(size: compact ? 10.5 : 11.5, weight: .medium))
                .foregroundStyle(tone == .idle ? Color.secondary : Color.primary)
                .lineLimit(1)
        }
        .padding(.horizontal, compact ? 8 : 10)
        .padding(.vertical, compact ? 3 : 4.5)
        .background(
            Capsule()
                .fill(GeminiPalette.cardFill(for: colorScheme, elevated: true))
        )
        .overlay(
            Capsule()
                .strokeBorder(
                    tone.isLive ? tone.dotColor.opacity(0.34) : GeminiPalette.hairlineBorder(for: colorScheme),
                    lineWidth: 1
                )
        )
    }
}

// MARK: - Lightweight Device Stage Halo (Pure Radial Gradient, Zero Blur Pass)

struct GeminiDeviceStageAura: View {
    let isRunning: Bool
    let acceptsInput: Bool

    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        let dark = colorScheme == .dark
        Ellipse()
            .fill(
                RadialGradient(
                    stops: isRunning
                        ? [
                            .init(color: GeminiPalette.cyan.opacity(dark ? 0.14 : 0.085), location: 0.0),
                            .init(color: GeminiPalette.indigo.opacity(dark ? 0.085 : 0.05), location: 0.42),
                            .init(color: GeminiPalette.violet.opacity(dark ? 0.04 : 0.025), location: 0.72),
                            .init(color: .clear, location: 1.0)
                        ]
                        : [
                            .init(color: GeminiPalette.indigo.opacity(dark ? 0.065 : 0.035), location: 0.0),
                            .init(color: .clear, location: 0.78)
                        ],
                    center: .center,
                    startRadius: 8,
                    endRadius: 230
                )
            )
            .frame(maxWidth: 460, maxHeight: 520)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

// MARK: - Clean Interactive Split Divider

struct GeminiNeonDividerHandle: View {
    let isDragging: Bool
    @State private var isHovered = false
    @Environment(\.colorScheme) private var colorScheme

    private var highlighted: Bool { isDragging || isHovered }

    var body: some View {
        let accent = GeminiPalette.accent(for: colorScheme)
        ZStack {
            Rectangle()
                .fill(highlighted ? accent.opacity(0.42) : GeminiPalette.hairlineBorder(for: colorScheme))
                .frame(height: 1)

            Capsule()
                .fill(highlighted ? accent : Color(nsColor: .tertiaryLabelColor).opacity(0.55))
                .frame(width: highlighted ? 48 : 34, height: 3)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(GeminiPalette.stageBackground(for: colorScheme))
        .animation(.easeOut(duration: 0.12), value: highlighted)
        .onHover { isHovered = $0 }
    }
}

// MARK: - Non-Blocking Toast Pill

struct GeminiToastBanner: View {
    let message: String
    var isError: Bool = false
    let onDismiss: () -> Void

    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        let accent = isError ? GeminiPalette.amber : GeminiPalette.accent(for: colorScheme)
        HStack(spacing: 8) {
            Image(systemName: isError ? "exclamationmark.triangle.fill" : "sparkles")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(accent)
            Text(message)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(.primary)
                .lineLimit(2)
            Button {
                onDismiss()
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(
            Capsule()
                .fill(GeminiPalette.cardFill(for: colorScheme, elevated: true))
        )
        .overlay(
            Capsule()
                .strokeBorder(accent.opacity(0.42), lineWidth: 1)
        )
        .shadow(color: Color.black.opacity(colorScheme == .dark ? 0.30 : 0.10), radius: 10, y: 3)
        .transition(.opacity)
    }
}

// MARK: - Clean Solid Progress Bar

struct GeminiGradientProgressBar: View {
    let value: Double
    let total: Double
    var highlighted: Bool = true

    @Environment(\.colorScheme) private var colorScheme

    private var fraction: Double {
        guard total > 0, value.isFinite, total.isFinite else { return 0 }
        return min(max(value / total, 0), 1)
    }

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule()
                    .fill(colorScheme == .dark ? Color.white.opacity(0.08) : Color.black.opacity(0.07))
                Capsule()
                    .fill(highlighted ? GeminiPalette.accent(for: colorScheme) : GeminiPalette.indigo.opacity(0.55))
                    .frame(width: max(fraction > 0 ? 6 : 0, proxy.size.width * fraction))
            }
        }
        .frame(height: 6)
    }
}
