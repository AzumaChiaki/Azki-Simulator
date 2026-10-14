import Foundation
import Observation

/// Shared across device instances; applies to the next request or connection.
@MainActor @Observable
final class ConnectionSettings {
    static let shared = ConnectionSettings()
    nonisolated static let defaultTimeoutSeconds = 30
    nonisolated static let timeoutRange = 1...300
    nonisolated static let defaultsKey = "simulator.connectionTimeoutSeconds"

    @ObservationIgnored private let defaults: UserDefaults
    var timeoutSeconds: Int {
        didSet {
            if !Self.timeoutRange.contains(timeoutSeconds) { timeoutSeconds = oldValue }
            defaults.set(timeoutSeconds, forKey: Self.defaultsKey)
        }
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        timeoutSeconds = Self.readTimeoutSeconds(from: defaults)
    }

    nonisolated static func readTimeoutSeconds(from defaults: UserDefaults = .standard) -> Int {
        guard let number = defaults.object(forKey: defaultsKey) as? NSNumber else { return defaultTimeoutSeconds }
        let value = number.doubleValue
        guard value.isFinite, value.rounded() == value,
              Double(timeoutRange.lowerBound)...Double(timeoutRange.upperBound) ~= value else { return defaultTimeoutSeconds }
        return Int(value)
    }
}

nonisolated struct SimulatorConnectionTimeout: LocalizedError, Sendable {
    var errorDescription: String? { "连接超时，请重试或调大“连接超时（秒）”。" }
}
