import Foundation

/// Register assignments follow best1503.c's ISPI CS3/page0 read path.
/// Sensor labels are scoped to firmware with observed reversible guest effects.
/// No calibrated physical-unit input is implied by the raw register interface.
nonisolated enum PMUADCDescriptors {
    static let channels = Array(0..<8)
    static let defaultRawValue = 21_000
    static let rawRange = 0...65_535

    static func purpose(_ channel: Int, profileID: String? = nil) -> String {
        if profileID == "q66@4.100.177", channel == 2 {
            return "影响固件电池电压和电量估算 · 单位为原始计数（raw）"
        }
        if profileID == "q66@4.100.177", channel == 5 {
            return "NTC 测温路径输入 · 原始计数（raw），尚未校准为 °C"
        }
        return "PMU 采样输入 \(channel) · 原始计数（raw），尚未换算为物理量"
    }

    static func title(_ channel: Int, profileID: String? = nil) -> String {
        if profileID == "q66@4.100.177", channel == 2 {
            return "电池电压采样 · 0x58"
        }
        if profileID == "q66@4.100.177", channel == 5 {
            return "NTC 温度采样 · 0x5B"
        }
        return "采样寄存器 " + String(format: "0x%02X", 0x56 + channel)
    }

    static func values(in response: SimulatorJSON?) -> [Int]? {
        guard case .array(let entries)? = response?["state"]?["values"], entries.count == 8 else { return nil }
        let values = entries.compactMap { entry -> Int? in
            guard case .number(let number) = entry, number.isFinite,
                  number.rounded() == number, (0...65535).contains(number) else { return nil }
            return Int(number)
        }
        return values.count == 8 ? values : nil
    }

    static func readStatus(_ channel: Int, response: SimulatorJSON?) -> String {
        guard channels.contains(channel), let state = response?["state"],
              case .number(let version)? = state["version"],
              case .array(let reads)? = state["readCount"], reads.count == 8,
              case .array(let versions)? = state["lastReadVersion"], versions.count == 8,
              case .number(let count) = reads[channel],
              case .number(let lastVersion) = versions[channel] else {
            return "尚未读取采样状态"
        }
        // SimulatorJSON stores numbers as Double. Do not infer an exact version
        // match after integers exceed its precise representation range.
        guard version.isFinite, version >= 0, version <= 9_007_199_254_740_991,
              lastVersion.isFinite, lastVersion >= 0, lastVersion <= version,
              count.isFinite, count >= 0 else { return "读取版本未知" }
        if count == 0 { return "固件尚未读取此采样" }
        if lastVersion == version { return "固件已读取当前采样；应用效果未确认" }
        return "固件仅读取过旧采样，等待当前输入"
    }

    static func readCount(_ channel: Int, response: SimulatorJSON?) -> String {
        guard channels.contains(channel),
              case .array(let counts)? = response?["state"]?["readCount"], counts.count == 8,
              case .number(let count) = counts[channel], count.isFinite,
              count.rounded() == count, (0...9_007_199_254_740_991).contains(count) else {
            return "尚无读取记录"
        }
        return "本次启动固件读取 \(Int64(count)) 次"
    }
}

/// Refreshing the observed hardware state must not discard unapplied edits.
nonisolated struct PMUADCDraft {
    var text = Array(repeating: "", count: 8)
    private(set) var observed: [Int]?

    var values: [Int]? {
        let parsed = text.compactMap(Int.init)
        return parsed.count == 8 && parsed.allSatisfy(PMUADCDescriptors.rawRange.contains) ? parsed : nil
    }
    var hasChanges: Bool { observed != nil && values != observed }

    mutating func receive(_ response: SimulatorJSON?) {
        guard let latest = PMUADCDescriptors.values(in: response) else {
            observed = nil; text = Array(repeating: "", count: 8); return
        }
        for index in PMUADCDescriptors.channels {
            if observed == nil || Int(text[index]) == observed?[index] {
                text[index] = String(latest[index])
            }
        }
        observed = latest
    }
    mutating func discardEdits() {
        if let observed { text = observed.map(String.init) }
    }
    mutating func useBootDefaults() {
        text = Array(repeating: String(PMUADCDescriptors.defaultRawValue), count: 8)
    }
}
