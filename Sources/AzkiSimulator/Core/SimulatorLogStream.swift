import Foundation
import Observation

nonisolated struct SimulatorLogBuffer: Sendable {
    static let maximumBytes = 262_144
    private(set) var text = ""
    private(set) var discardedUTF16 = 0
    private var pending = Data()

    mutating func reset() { text = ""; pending = Data(); discardedUTF16 = 0 }
    mutating func append(_ bytes: Data) {
        var data = pending; data.append(bytes); pending.removeAll(keepingCapacity: true)
        // Keep a final incomplete UTF-8 scalar for the next byte chunk. Invalid
        // complete input is replaced; no bytes outside this bounded chunk load.
        if let lead = data.indices.reversed().first(where: { data[$0] & 0xc0 != 0x80 }) {
            let byte = data[lead]
            let expected = (0xc2...0xdf).contains(byte) ? 2 : (0xe0...0xef).contains(byte) ? 3 : (0xf0...0xf4).contains(byte) ? 4 : 1
            if data.distance(from: lead, to: data.endIndex) < expected {
                pending = Data(data.suffix(from: lead))
                data.removeSubrange(lead...)
            }
        }
        text += String(decoding: data, as: UTF8.self)
        if text.utf8.count > Self.maximumBytes {
            let bounded = Array(text.utf8.suffix(Self.maximumBytes))
            let first = bounded.firstIndex(where: { $0 & 0xc0 != 0x80 }) ?? bounded.endIndex
            let retained = String(decoding: bounded[first...], as: UTF8.self)
            discardedUTF16 += text.utf16.count - retained.utf16.count
            text = retained
        }
    }
}

/// A dedicated read-only connection keeps log I/O off the frame/input channel.
/// Polling is single-flight and at most four requests/UI batches per second.
@MainActor @Observable final class SimulatorLogStreamModel {
    private(set) var text = ""
    private(set) var isFollowing = false
    private(set) var detail = "暂无日志"
    private(set) var revision: UInt64 = 0
    private(set) var epoch: UInt64 = 0
    private(set) var discardedUTF16 = 0
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var connection: SimulatorSessionConnection?
    @ObservationIgnored private var generation = UUID()

    func start(directory: URL, sessionID: UUID, name: String) {
        stop()
        text = ""; revision &+= 1; epoch &+= 1; discardedUTF16 = 0; detail = "等待日志"; isFollowing = true
        let token = generation
        task = Task { [weak self] in
            var buffer = SimulatorLogBuffer()
            var cursor: SimulatorJSON = .null
            var skipped: Double = 0
            do {
                let client = try await Task.detached { try SimulatorSessionConnection(directoryURL: directory) }.value
                guard let self, self.generation == token, !Task.isCancelled else { await client.close(); return }
                guard client.manifest.id == sessionID else { await client.close(); throw SimulatorError.invalidSession("日志会话已改变") }
                self.connection = client
                defer { Task { await client.close() } }
                while !Task.isCancelled, self.generation == token {
                    let value = try await client.command("logs", arguments: ["name": .string(name), "stream": .bool(true),
                        "limit": .number(16_384), "cursor": cursor])
                    guard self.generation == token, !Task.isCancelled else { return }
                    guard value["version"] == .number(2), let encoded = value["dataBase64"]?.string,
                          encoded.utf8.count <= 44_000, let bytes = Data(base64Encoded: encoded), bytes.count <= 32_768,
                          let reset = value["reset"]?.bool, let missing = value["missing"]?.bool else {
                        throw SimulatorError.transport("该会话尚不支持增量日志；请使用更新后的桥接服务")
                    }
                    if reset { buffer.reset(); self.epoch &+= 1 }
                    if missing || value["streamID"] == .null {
                        cursor = .null
                    } else {
                        guard let stream = value["streamID"]?.string, stream.utf8.count <= 64,
                              case .number(let offset) = value["nextOffset"], offset.isFinite,
                              offset >= 0, offset <= 9_007_199_254_740_991, offset.rounded() == offset else {
                            throw SimulatorError.transport("日志游标无效")
                        }
                        cursor = .object(["streamID": .string(stream), "offset": .number(offset)])
                    }
                    if case .number(let count) = value["skippedBytes"], count >= 0, count.isFinite { skipped += count }
                    buffer.append(bytes)
                    self.discardedUTF16 = buffer.discardedUTF16
                    if self.text != buffer.text { self.text = buffer.text; self.revision &+= 1 }
                    self.detail = missing ? "等待日志文件" : skipped > 0 ? "实时刷新 · 已省略较早日志" : "实时刷新 · 最多保留 256 KiB"
                    try await Task.sleep(for: .milliseconds(250))
                }
            } catch is CancellationError { }
            catch {
                guard let self, self.generation == token else { return }
                self.detail = error.localizedDescription
            }
            guard let self, self.generation == token else { return }
            self.isFollowing = false
        }
    }
    func stop() {
        generation = UUID(); task?.cancel(); task = nil; isFollowing = false
        if let connection { Task { await connection.close() } }
        connection = nil
    }
}
