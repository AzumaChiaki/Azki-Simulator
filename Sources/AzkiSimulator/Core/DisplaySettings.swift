import Foundation
import Observation

/// Shared display preferences across all simulator windows, screenshots and recordings.
@MainActor @Observable
final class DisplaySettings {
    static let shared = DisplaySettings()
    nonisolated static let defaultClipToScreenShape = true
    nonisolated static let defaultsKey = "simulator.clipToScreenShape"

    @ObservationIgnored private let defaults: UserDefaults

    /// Clips the live device canvas, screenshots (transparent PNG corners) and
    /// screen recordings to the physical screen silhouette of each device.
    var clipToScreenShape: Bool {
        didSet {
            defaults.set(clipToScreenShape, forKey: Self.defaultsKey)
        }
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        clipToScreenShape = Self.readClipToScreenShape(from: defaults)
    }

    nonisolated static func readClipToScreenShape(from defaults: UserDefaults = .standard) -> Bool {
        guard let value = defaults.object(forKey: defaultsKey) as? Bool else {
            return defaultClipToScreenShape
        }
        return value
    }
}
