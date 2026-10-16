import Foundation

/// Per-instance firmware settings stored by the bundled helper. Properties are
/// written into the firmware's persist.db at the next boot; nil restores the
/// firmware's own default. The clock decides the boot wall time.
nonisolated struct InstanceFirmwareSettings: Codable, Equatable, Sendable {
    enum ClockMode: String, Codable, Sendable { case host, offset, firmware }
    struct Clock: Codable, Equatable, Sendable {
        var mode: ClockMode
        var offsetSeconds: Int?
    }
    /// Board inputs of the instance's hardware preset. The preset decides which
    /// devices exist, so only options the emulated board has are offered.
    enum Charger: String, Codable, Sendable { case auto, unplugged, charging, full }
    enum Gauge: String, Codable, Sendable { case auto, hp4560, hp4570 }
    struct Battery: Codable, Equatable, Sendable {
        /// Boot level in percent; the helper turns it into the firmware's own
        /// saved state of charge and battery voltage sample.
        var level: Int?
        var sampleRaw: Int?
        var charger: Charger = .auto
        var gauge: Gauge = .auto
        init() {}
        // The helper omits "auto" values, so every field is optional on input.
        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            level = try values.decodeIfPresent(Int.self, forKey: .level)
            sampleRaw = try values.decodeIfPresent(Int.self, forKey: .sampleRaw)
            charger = try values.decodeIfPresent(Charger.self, forKey: .charger) ?? .auto
            gauge = try values.decodeIfPresent(Gauge.self, forKey: .gauge) ?? .auto
        }
    }
    /// A preset whose firmware the battery inputs verifiably drive.
    struct BatteryInputs: Decodable, Equatable, Sendable {
        let modelPrefix: String
        let level: Bool
        let charger: Bool
    }
    /// Wearer input for the emulated motion and optical sensors: the firmware
    /// counts the walked steps and measures the pulse itself.
    struct Health: Codable, Equatable, Sendable {
        var walkSteps: Int?
        var cadence: Int?
        var heartRate: Int?
        var isEmpty: Bool { walkSteps == nil && cadence == nil && heartRate == nil }
        init() {}
        private enum CodingKeys: String, CodingKey { case walkSteps, cadence, heartRate, steps }
        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            walkSteps = try values.decodeIfPresent(Int.self, forKey: .walkSteps)
                ?? values.decodeIfPresent(Int.self, forKey: .steps)
            cadence = try values.decodeIfPresent(Int.self, forKey: .cadence)
            heartRate = try values.decodeIfPresent(Int.self, forKey: .heartRate)
        }
        func encode(to encoder: Encoder) throws {
            var values = encoder.container(keyedBy: CodingKeys.self)
            try values.encodeIfPresent(walkSteps, forKey: .walkSteps)
            try values.encodeIfPresent(cadence, forKey: .cadence)
            try values.encodeIfPresent(heartRate, forKey: .heartRate)
        }
    }
    struct HealthInputs: Decodable, Equatable, Sendable {
        let modelPrefix: String
        let steps: Bool
        let heartRate: Bool
    }
    var schema = 1
    var properties: [String: String?] = [:]
    var clock = Clock(mode: .host, offsetSeconds: nil)
    /// An explicit "off" state is absent so the emulated default is kept.
    var battery: Battery?
    var health: Health?

    static let maximumOffsetSeconds = 100 * 366 * 86_400
    static let maximumSampleRaw = 65_535
    static let maximumSteps = 10_000_000
    static let maximumHeartRate = 255
    static let cadenceRange = 40...200
    static func validKey(_ key: String) -> Bool {
        key.hasPrefix("persist.") && key.utf8.count <= 128 && key.count > 8
            && key.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45, 46, 95].contains($0) }
    }
    static func validValue(_ value: String) -> Bool {
        value.count <= 1_024 && !value.contains(where: { "\0\n\r\t".contains($0) })
    }

    /// The value the firmware will use: an override, an explicit reset (nil),
    /// or no change (absent).
    func override(_ key: String) -> String?? { properties[key] }
    mutating func set(_ key: String, _ value: String??) {
        if let value { properties[key] = value } else { properties.removeValue(forKey: key) }
    }
}

nonisolated struct InstanceFirmwareSettingsReceipt: Decodable, Sendable {
    struct Observed: Decodable, Sendable {
        let properties: [String: String]
        let recordedAtUnix: Int?
    }
    let ok: Bool
    let settings: InstanceFirmwareSettings
    let observed: Observed?
    let batteryInputs: [InstanceFirmwareSettings.BatteryInputs]?
    let healthInputs: [InstanceFirmwareSettings.HealthInputs]?

    func healthInputs(for model: String?) -> InstanceFirmwareSettings.HealthInputs? {
        guard let model else { return nil }
        return healthInputs?.first { model.hasPrefix($0.modelPrefix) }
    }

    func batteryInputs(for model: String?) -> InstanceFirmwareSettings.BatteryInputs? {
        guard let model else { return nil }
        return batteryInputs?.first { model.hasPrefix($0.modelPrefix) }
    }

    static func decode(_ data: Data) throws -> Self {
        let value: Self = try FirmwareLibraryValidation.decode(data)
        let settings = value.settings
        let battery = settings.battery
        let health = settings.health
        guard value.ok, settings.schema == 1,
              settings.properties.count <= 512,
              settings.properties.allSatisfy({ InstanceFirmwareSettings.validKey($0.key)
                  && ($0.value.map(InstanceFirmwareSettings.validValue) ?? true) }),
              (battery?.sampleRaw.map { (0...InstanceFirmwareSettings.maximumSampleRaw).contains($0) } ?? true),
              (battery?.level.map { (0...100).contains($0) } ?? true),
              (health?.walkSteps.map { (0...InstanceFirmwareSettings.maximumSteps).contains($0) } ?? true),
              (health?.cadence.map { InstanceFirmwareSettings.cadenceRange.contains($0) } ?? true),
              (health?.heartRate.map { (0...InstanceFirmwareSettings.maximumHeartRate).contains($0) } ?? true),
              (value.observed?.properties.count ?? 0) <= 4_096 else {
            throw SimulatorError.invalidSession("实例固件设置内容无效")
        }
        return value
    }
}

/// Firmware semantics verified on Band 9 3.1.32; the same persist.db keys are
/// used across the YAFFS-based Band 9 / 9 Pro / 10 firmware family.
nonisolated enum InstanceFirmwareSettingKeys {
    static let locale = "persist.locale"
    static let timeFormat = "persist.system_time_format"   // "true" = 12-hour
    static let timeZone = "persist.system_tz"              // IANA zone name
    static let languages: [(String, String)] = [
        ("zh_CN", "简体中文"), ("zh_TW", "繁體中文（台灣）"), ("zh_HK", "繁體中文（香港）"),
        ("en_US", "English (US)"), ("en_GB", "English (UK)"), ("ja_JP", "日本語"), ("ko_KR", "한국어"),
        ("de_DE", "Deutsch"), ("fr_FR", "Français"), ("es_ES", "Español"), ("it_IT", "Italiano"),
        ("pt_BR", "Português (Brasil)"), ("ru_RU", "Русский"), ("th_TH", "ไทย"), ("vi_VN", "Tiếng Việt")]
    static let timeZones = ["Asia/Shanghai", "Asia/Hong_Kong", "Asia/Taipei", "Asia/Tokyo", "Asia/Seoul",
        "Asia/Singapore", "Asia/Bangkok", "Asia/Kolkata", "Asia/Dubai", "Europe/London", "Europe/Paris",
        "Europe/Berlin", "Europe/Moscow", "America/New_York", "America/Chicago", "America/Denver",
        "America/Los_Angeles", "America/Sao_Paulo", "Australia/Sydney", "Pacific/Auckland", "UTC"]
}
