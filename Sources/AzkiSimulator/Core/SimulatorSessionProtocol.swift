import Foundation

nonisolated struct SimulatorSessionManifest: Codable, Equatable, Sendable {
    let version: Int
    let id: UUID
    let socket: String
    let framebuffer: String
    let width: Int
    let height: Int
    let capabilities: [String]
    var displayGeometry: SimulatorDisplayGeometry? { SimulatorDisplayGeometry(width: width, height: height) }

    static func decode(_ data: Data) throws -> Self {
        guard data.count <= 16_384 else { throw SimulatorError.invalidSession("清单超过 16 KiB") }
        let result = try JSONDecoder().decode(Self.self, from: data)
        guard result.version == 1, result.socket == "control.sock", result.framebuffer == "frame.raw",
              result.displayGeometry != nil,
              result.capabilities.count <= 32, result.capabilities.allSatisfy({ $0.utf8.count <= 64 }) else {
            throw SimulatorError.invalidSession("版本、小米手环屏幕尺寸或受限文件名不匹配")
        }
        return result
    }
}

nonisolated enum SimulatorJSON: Codable, Equatable, Sendable {
    case string(String), number(Double), bool(Bool), object([String: Self]), array([Self]), null

    init(from decoder: Decoder) throws {
        let value = try decoder.singleValueContainer()
        if value.decodeNil() { self = .null }
        else if let decoded = try? value.decode(Bool.self) { self = .bool(decoded) }
        else if let decoded = try? value.decode(String.self) { self = .string(decoded) }
        else if let decoded = try? value.decode(Double.self) { self = .number(decoded) }
        else if let decoded = try? value.decode([String: Self].self) { self = .object(decoded) }
        else { self = .array(try value.decode([Self].self)) }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .bool(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .null: try container.encodeNil()
        }
    }

    subscript(key: String) -> Self? { if case .object(let object) = self { object[key] } else { nil } }
    var string: String? { if case .string(let value) = self { value } else { nil } }
    var bool: Bool? { if case .bool(let value) = self { value } else { nil } }
    var prettyDescription: String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return (try? encoder.encode(self)).flatMap { String(data: $0, encoding: .utf8) } ?? ""
    }
}

nonisolated struct SimulatorBridgeRequest: Encodable, Sendable {
    let version = 1
    let id: String
    let command: String
    let arguments: [String: SimulatorJSON]

    init(id: UUID, command: String, arguments: [String: SimulatorJSON]) {
        self.id = id.uuidString.lowercased()
        self.command = command
        self.arguments = arguments
    }
}

nonisolated struct SimulatorBridgeResponse: Decodable, Sendable {
    struct Failure: Decodable, Sendable { let code: String; let message: String }
    let ok: Bool
    let result: SimulatorJSON?
    let error: Failure?

    func checkedResult() throws -> SimulatorJSON {
        guard ok else { throw SimulatorError.remote(error.map { "\($0.message) [\($0.code)]" } ?? "本机桥接拒绝了请求") }
        return result ?? .null
    }
}
