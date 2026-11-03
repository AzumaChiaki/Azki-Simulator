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

/// A complete hardware input snapshot. This is not evidence that the guest
/// firmware has consumed the input or displayed a resulting measurement.
nonisolated struct SimulatorLiveState: Equatable, Sendable {
    static let addStepsRange = 1...100_000
    static let batteryLevelRange = 0...100

    let supported: [String: Bool]
    let notes: [String: String]
    let state: [String: SimulatorJSON]

    init?(_ json: SimulatorJSON?) {
        let supportKeys: Set<String> = ["steps", "heartRate", "charger", "batteryLevel", "batteryRaw", "temperature", "clock"]
        let stateKeys: Set<String> = ["walkSteps", "walked", "cadence", "heartRate", "worn", "charger", "batteryRaw", "temperatureC", "clock"]
        let required: Set<String> = ["supported", "notes", "state"]
        guard case .object(let object)? = json, required.isSubset(of: Set(object.keys)),
              Set(object.keys).isSubset(of: required.union(["sensorObservation"])),
              case .object(let supported)? = object["supported"],
              case .object(let state)? = object["state"], case .object(let notes)? = object["notes"],
              supportKeys.isSubset(of: Set(supported.keys)), Set(supported.keys).isSubset(of: supportKeys.union(["orientation", "motionRaw"])),
              stateKeys.isSubset(of: Set(state.keys)), Set(state.keys).isSubset(of: stateKeys.union(["orientation", "motionRaw"])),
              supported.values.allSatisfy({ $0.bool != nil }), notes.values.allSatisfy({ $0.string != nil }),
              (supported["orientation"] != nil) == (state["orientation"] != nil),
              (supported["motionRaw"] != nil) == (state["motionRaw"] != nil),
              supported["clock"] == .bool(false), state["clock"] == .null else { return nil }
        // Sensor-bus counters are not guest-consumption evidence. This view
        // does not consume or present them; Runtime validates their full ABI.
        if let observation = object["sensorObservation"] {
            guard case .object = observation else { return nil }
        }
        func integer(_ key: String, _ range: ClosedRange<Double>) -> Bool {
            guard case .number(let number)? = state[key] else { return false }
            return number.isFinite && number.rounded() == number && range.contains(number)
        }
        func absent(_ keys: [String]) -> Bool { keys.allSatisfy { state[$0] == .null } }
        guard supported["steps"] == .bool(true)
                ? integer("walkSteps", 0...9_007_199_254_740_991) && integer("walked", 0...9_007_199_254_740_991) && integer("cadence", 40...200)
                : absent(["walkSteps", "walked", "cadence"]),
              supported["heartRate"] == .bool(true)
                ? integer("heartRate", 0...255) && state["worn"]?.bool != nil : absent(["heartRate", "worn"]),
              supported["charger"] == .bool(true)
                ? state["charger"]?.string.flatMap(SimulatorLiveCharger.init(rawValue:)) != nil : absent(["charger"]),
              supported["batteryRaw"] == .bool(true) ? integer("batteryRaw", 0...65_535) : absent(["batteryRaw"]),
              supported["batteryLevel"] != .bool(true) || supported["batteryRaw"] == .bool(true) else { return nil }
        if supported["temperature"] == .bool(true) {
            guard case .number(let number)? = state["temperatureC"], number.isFinite, (-40...125).contains(number) else { return nil }
        } else if !absent(["temperatureC"]) { return nil }
        if let orientation = state["orientation"], orientation != .null {
            guard supported["orientation"] == .bool(true), case .object(let angles) = orientation,
                  Set(angles.keys) == ["rollDegrees", "pitchDegrees"],
                  angles.values.allSatisfy({ value in
                      guard case .number(let number) = value else { return false }
                      return number.isFinite && (-180...180).contains(number)
                  }) else { return nil }
        }
        if let motion = state["motionRaw"] {
            if motion == .null {
                guard supported["motionRaw"] == .bool(false) else { return nil }
            } else {
                guard supported["motionRaw"] == .bool(true), case .object(let raw) = motion else { return nil }
                if raw != ["mode": .string("auto")] {
                    guard Set(raw.keys) == ["mode", "accel", "gyro"], raw["mode"] == .string("raw") else { return nil }
                    for key in ["accel", "gyro"] {
                        guard case .array(let axes)? = raw[key], axes.count == 3,
                              axes.allSatisfy({ value in
                                  guard case .number(let number) = value else { return false }
                                  return number.isFinite && number.rounded() == number && (-32_768...32_767).contains(number)
                              }) else { return nil }
                    }
                }
            }
        }
        self.supported = supported.compactMapValues(\.bool)
        self.notes = notes.compactMapValues(\.string)
        self.state = state
    }

    func supports(_ key: String) -> Bool { supported[key] == true }

    func int(_ key: String) -> Int? {
        guard case .number(let value)? = state[key], value.isFinite, value.rounded() == value,
              abs(value) <= 9_007_199_254_740_991 else { return nil }
        return Int(value)
    }

    func bool(_ key: String) -> Bool? { state[key]?.bool }

    func number(_ key: String) -> Double? {
        guard case .number(let value)? = state[key], value.isFinite else { return nil }
        return value
    }

    var charger: SimulatorLiveCharger? { state["charger"]?.string.flatMap(SimulatorLiveCharger.init(rawValue:)) }
    /// Steps still to walk at the current cadence.
    var stepsRemaining: Int { max(0, (int("walkSteps") ?? 0) - (int("walked") ?? 0)) }
}

nonisolated struct SimulatorLiveOutcomeUnknown: LocalizedError, Sendable {
    var errorDescription: String? { "硬件输入可能已更新，但未得到有效确认；请等待刷新，不要重复提交。[live_outcome_unknown]" }
}

/// Ordinary live-set acknowledgement. Orientation has a separate receipt and
/// is deliberately not decoded as a complete device-data snapshot here.
nonisolated struct SimulatorLiveWriteReceipt: Equatable, Sendable {
    let snapshot: SimulatorLiveState?
    let guestObserved = false

    init(_ json: SimulatorJSON) throws {
        if let legacy = SimulatorLiveState(json) {
            snapshot = legacy
            return
        }
        guard case .object(let object) = json,
              object["accepted"] == .bool(true), object["guestObserved"] == .bool(false) else {
            throw SimulatorLiveOutcomeUnknown()
        }
        if Set(object.keys) == ["accepted", "guestObserved", "stateUnavailable"], object["stateUnavailable"] == .bool(true) {
            snapshot = nil
        } else if Set(object.keys) == ["accepted", "guestObserved", "state"], let state = SimulatorLiveState(object["state"]) {
            snapshot = state
        } else { throw SimulatorLiveOutcomeUnknown() }
    }
}

/// Pure delivery classification; no retry and no I/O. A complete request with
/// no acknowledgement must not look like a definitely unapplied input.
nonisolated enum SimulatorLiveDelivery {
    static func error(_ error: Error, command: String, submitted: Bool, receivedReply: Bool) -> Error {
        command == SimulatorLiveCommands.set && submitted && !receivedReply ? SimulatorLiveOutcomeUnknown() : error
    }
}

nonisolated enum SimulatorLiveFreshness {
    static func failureMessage(_ description: String, hasSnapshot: Bool) -> String {
        if hasSnapshot { return "当前值为上次读取的数据，可能已过期；刷新失败：\(description)" }
        if description.contains("[unsupported") || description.contains("不受支持") {
            return "当前运行的核心不提供实时输入，修改将在下次开机生效。"
        }
        return "暂时读不到实时输入：\(description)"
    }
}

nonisolated struct SimulatorLiveApplyProgress {
    var hardwareAccepted = false
    var nextBootSaved = false
    var stateUnavailable = false

    func message(failure: String? = nil) -> String? {
        var parts: [String] = []
        if hardwareAccepted { parts.append("模拟硬件已接受输入；固件是否采用请以设备画面为准") }
        if stateUnavailable { parts.append("暂时无法回读，当前值可能已过期，请等待刷新，不要重复提交") }
        if nextBootSaved { parts.append("已保存为下次开机的值") }
        if let failure {
            if hardwareAccepted && !nextBootSaved { parts.append("下次开机的值未确认保存：\(failure)") }
            else { parts.append(failure) }
        }
        return parts.isEmpty ? nil : parts.joined(separator: "。")
    }
}
