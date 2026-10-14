import Foundation

/// Public, synthetic device fields only. Pairing keys remain in device snapshots.
nonisolated struct SimulatorVirtualIdentity: Codable, Equatable, Sendable {
    var serial: String
    var address: String

    static func generated(for id: UUID) -> Self {
        let digits = id.uuidString.replacingOccurrences(of: "-", with: "").uppercased()
        let bytes = stride(from: 0, to: 10, by: 2).map { offset in
            String(digits.dropFirst(offset).prefix(2))
        }
        return Self(serial: "AZKI-" + digits.suffix(16), address: (["02"] + bytes).joined(separator: ":"))
    }

    static func serialLimit(model: String) -> Int {
        if model.hasPrefix("miwear.watch.n67") { return 21 }
        if model.hasPrefix("miwear.watch.n66") || model.hasPrefix("miwear.watch.o66") { return 24 }
        return 31
    }

    func validated(model: String) throws -> Self {
        let value = Self(serial: serial.trimmingCharacters(in: .whitespacesAndNewlines),
                         address: address.trimmingCharacters(in: .whitespacesAndNewlines).uppercased())
        guard !value.serial.isEmpty, value.serial.utf8.allSatisfy({ (33...126).contains($0) }),
              value.serial.utf8.count <= Self.serialLimit(model: model) else {
            throw SimulatorInstanceStoreError.invalid("序列号须为 1–\(Self.serialLimit(model: model)) 位可见英文字符，不能包含空格或换行")
        }
        guard value.address.range(of: #"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$"#, options: .regularExpression) != nil else {
            throw SimulatorInstanceStoreError.invalid("蓝牙地址须为六组十六进制数，例如 12:34:56:78:9A:BC")
        }
        return value
    }
}
