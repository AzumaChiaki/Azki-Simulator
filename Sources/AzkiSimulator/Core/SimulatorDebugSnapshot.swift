import Foundation

/// A bounded copy of already-observed public session data. Construct on the UI
/// actor from one session model; encoding performs no file, process or socket I/O.
nonisolated struct SimulatorDebugSnapshot: Encodable, Equatable, Sendable {
    static let maximumLogBytes = 8_192
    static let maximumEncodedBytes = 65_536

    struct Frame: Encodable, Equatable, Sendable {
        let sequence: UInt32
        let width: Int
        let height: Int
    }
    struct Log: Encodable, Equatable, Sendable {
        let name: String?
        let text: String
        let truncated: Bool
    }

    let schema = "azki.simulator.debug-snapshot.v1"
    let capturedAt: Date
    let isAttached: Bool
    let sessionID: String?
    let statusSessionID: String?
    let statusIdentityMatchesSession: Bool?
    let profileID: String?
    let approvalDigest: String?
    let backend: String?
    let reportedState: String?
    let adapterState: String?
    let hostProcessRunning: Bool?
    let guestInstallSupported: Bool?
    let reportedBootVerified: Bool?
    let guestHealth: String?
    let generation: UInt64?
    let processID: Int?
    let maxRuntimeSeconds: Int?
    let reportedError: String?
    let capabilities: [String]
    let capabilitiesLimited: Bool
    let frame: Frame?
    let log: Log?
    let interpretation = [
        "状态由本机会话桥报告；快照时间是宿主采集时间，状态、画面和日志并非原子采样。",
        "触摸 accepted 仅表示宿主输入通道接收，不等于客体 ACK 或界面已响应。",
        "frame sequence 是已观测画面序号；帧更新次数不等于 guest FPS。",
        "guestInstallSupported 是能力声明；暂存包不等于已在客体安装。"
    ]

    init(manifest: SimulatorSessionManifest?, status: SimulatorJSON?, frame: SimulatorFrame?,
         isAttached: Bool, logName: String? = nil, logText: String? = nil, capturedAt: Date = Date()) {
        self.capturedAt = capturedAt
        self.isAttached = isAttached
        sessionID = manifest?.id.uuidString.lowercased()
        statusSessionID = Self.text(status?["id"], maximumBytes: 64)
        if let manifest, status?["id"] != nil {
            statusIdentityMatchesSession = statusSessionID.flatMap { UUID(uuidString: $0) } == manifest.id
        } else { statusIdentityMatchesSession = nil }
        // A stale status for another session must not acquire this manifest's
        // identity. A legacy status without an id remains explicitly unconfirmed.
        let observed = statusIdentityMatchesSession == false ? nil : status
        profileID = Self.text(observed?["profileID"], maximumBytes: 96)
        let digest = Self.text(observed?["approvalDigest"], maximumBytes: 64)
        approvalDigest = digest.flatMap { value in
            value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } ? value : nil
        }
        backend = Self.text(observed?["backend"], maximumBytes: 64)
        reportedState = Self.text(observed?["state"], maximumBytes: 32)
        adapterState = Self.text(observed?["adapterState"], maximumBytes: 32)
        hostProcessRunning = observed?["hostProcessRunning"]?.bool
        guestInstallSupported = observed?["guestInstallSupported"]?.bool
        reportedBootVerified = observed?["bootVerified"]?.bool
        guestHealth = Self.text(observed?["guestHealth"], maximumBytes: 64)
        generation = Self.integer(observed?["generation"], range: 0...9_007_199_254_740_991).map { UInt64($0) }
        processID = Self.integer(observed?["pid"], range: 2...Int(Int32.max))
        maxRuntimeSeconds = Self.integer(observed?["maxRuntimeSeconds"], range: StandaloneRunLimits.unlimited...StandaloneRunLimits.maximumSeconds)
        reportedError = Self.text(observed?["error"], maximumBytes: 1_024)
        let advertised: [SimulatorJSON]
        if case .array(let values)? = observed?["capabilities"] { advertised = values }
        else { advertised = (manifest?.capabilities ?? []).map(SimulatorJSON.string) }
        let selected = advertised.prefix(32).compactMap { Self.text($0, maximumBytes: 64) }
        capabilities = Array(Set(selected)).sorted()
        capabilitiesLimited = advertised.count > 32 || selected.count < min(advertised.count, 32)
        if let frame, frame.sequence > 0, frame.sequence & 1 == 0,
           SimulatorDisplayGeometry(width: frame.width, height: frame.height) != nil,
           manifest == nil || (frame.width == manifest?.width && frame.height == manifest?.height) {
            self.frame = Frame(sequence: frame.sequence, width: frame.width, height: frame.height)
        } else { self.frame = nil }
        if let logText {
            let bytes = Array(logText.utf8.suffix(Self.maximumLogBytes))
            // Drop a split leading UTF-8 scalar; never invent replacement bytes
            // that can push a bounded excerpt beyond its byte budget.
            let start = bytes.firstIndex(where: { $0 & 0xc0 != 0x80 }) ?? bytes.endIndex
            log = Log(name: logName.flatMap { Self.text(.string($0), maximumBytes: 64) },
                      text: String(decoding: bytes[start...], as: UTF8.self),
                      truncated: logText.utf8.count > Self.maximumLogBytes)
        } else { log = nil }
    }

    func encodedJSON() throws -> Data {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        let data = try encoder.encode(self)
        guard data.count <= Self.maximumEncodedBytes else {
            throw SimulatorError.transport("调试快照超过 64 KiB 导出限制")
        }
        return data
    }

    private static func text(_ value: SimulatorJSON?, maximumBytes: Int) -> String? {
        guard let text = value?.string, !text.isEmpty, text.utf8.count <= maximumBytes,
              !text.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else { return nil }
        return text
    }

    private static func integer(_ value: SimulatorJSON?, range: ClosedRange<Int>) -> Int? {
        guard case .number(let number)? = value, number.isFinite, number.rounded(.towardZero) == number,
              number >= Double(range.lowerBound), number <= Double(range.upperBound) else { return nil }
        return Int(number)
    }
}
