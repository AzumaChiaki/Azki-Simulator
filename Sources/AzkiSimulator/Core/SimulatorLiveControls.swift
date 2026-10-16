import Foundation

/// Live board inputs of a running session (live-get / live-set). The core
/// reports which inputs drive this preset's firmware; only those are offered.
nonisolated enum SimulatorLiveCommands {
    static let get = "live-get"
    static let set = "live-set"
    static let all: Set<String> = [get, set]
}

nonisolated enum SimulatorLiveCharger: String, CaseIterable, Sendable {
    case unplugged, charging, full
    var title: String {
        switch self {
        case .unplugged: "未接充电器"
        case .charging: "充电中"
        case .full: "已充满"
        }
    }
}

/// One live-get / live-set reply: the support matrix, the reason an input is
/// unavailable, and the core's state after the last change.
nonisolated struct SimulatorLiveState: Equatable, Sendable {
    static let addStepsRange = 1...100_000
    static let batteryLevelRange = 0...100

    let supported: [String: Bool]
    let notes: [String: String]
    let state: [String: SimulatorJSON]

    init?(_ json: SimulatorJSON?) {
        guard case .object(let supported)? = json?["supported"], case .object(let state)? = json?["state"] else { return nil }
        self.supported = supported.compactMapValues(\.bool)
        if case .object(let notes)? = json?["notes"] { self.notes = notes.compactMapValues(\.string) } else { notes = [:] }
        self.state = state
    }

    func supports(_ key: String) -> Bool { supported[key] == true }

    func int(_ key: String) -> Int? {
        guard case .number(let value)? = state[key], value.isFinite, value.rounded() == value,
              abs(value) <= 9_007_199_254_740_991 else { return nil }
        return Int(value)
    }

    func number(_ key: String) -> Double? {
        guard case .number(let value)? = state[key], value.isFinite else { return nil }
        return value
    }

    var charger: SimulatorLiveCharger? { state["charger"]?.string.flatMap(SimulatorLiveCharger.init(rawValue:)) }
    /// Steps still to walk at the current cadence.
    var stepsRemaining: Int { max(0, (int("walkSteps") ?? 0) - (int("walked") ?? 0)) }
}
