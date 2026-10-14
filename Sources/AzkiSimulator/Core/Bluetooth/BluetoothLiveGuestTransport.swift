import Foundation
import Combine

/// IPC is supplied by the instance runtime. It accepts a complete JSON request
/// and returns its `result` object, throwing on a peer error envelope.
@MainActor final class BluetoothLiveGuestTransport: BluetoothGuestTransport {
    typealias RPC = @MainActor ([String: Any]) async throws -> [String: Any]
    let rpc: RPC
    private(set) var bridgeID: String?
    private(set) var maximumATTMTU = 23
    var identityFingerprint: String { bridgeID ?? "" } // Session identity; not a digest.
    var notificationHandler: ((BluetoothGuestNotification) -> Void)?
    private var sessionID: UUID?
    private var polling: Task<Void, Never>?
    var onFailure: ((String) -> Void)?
    var onStatus: ((String?) -> Void)?
    private let requestSeconds: TimeInterval

    init(requestSeconds: TimeInterval = 30, rpc: @escaping RPC) {
        self.rpc = rpc; self.requestSeconds = requestSeconds
    }

    private func call(_ command: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
        var request = fields
        request["schema"] = 1; request["requestID"] = UUID().uuidString.lowercased(); request["command"] = command
        if command != "external-open" {
            guard let bridgeID else { throw BluetoothAdapterError.unavailable("外部蓝牙已关闭") }
            request["bridgeID"] = bridgeID
        }
        return try await rpc(request)
    }

    func prepare(localName: String) async throws -> BluetoothPeripheralConfiguration {
        let deadline = ProcessInfo.processInfo.systemUptime + requestSeconds
        var opened: [String: Any]
        while true {
            try Task.checkCancellation()
            do {
                opened = try await call("external-open", ["connectionTimeoutSeconds": Int(requestSeconds)])
                break
            } catch SimulatorError.remote(let message) where message.contains("[not_running]") {
                // This complete rejection means no bridge was opened. Native
                // process startup can precede the runner's private endpoint.
                guard ProcessInfo.processInfo.systemUptime < deadline else { throw SimulatorError.remote(message) }
                try await Task.sleep(for: .milliseconds(100))
            }
        }
        guard let identity = opened["bridgeID"] as? String else { throw BluetoothAdapterError.unavailable("Guest 未返回蓝牙连接") }
        bridgeID = identity
        while true {
            try Task.checkCancellation()
            let status = try await call("external-status")
            if status["state"] as? String == "failed" { throw BluetoothAdapterError.unavailable(status["error"] as? String ?? "Guest 蓝牙启动失败") }
            if status["state"] as? String == "ready" {
                maximumATTMTU = status["mtu"] as? Int ?? 23
                let services = (status["services"] as? [[String: Any]] ?? []).compactMap { service -> BluetoothServiceDefinition? in
                    guard let key = service["key"] as? String, let uuid = service["uuid"] as? String else { return nil }
                    // The native macOS peripheral rejects these standard services
                    // with CBErrorUUIDNotAllowed (observed in radio-08/09/10). Keep them
                    // accessible through guest ATT, but do not publish it on Mac.
                    if ["180a", "180f", "1812", "0000180a-0000-1000-8000-00805f9b34fb",
                        "0000180f-0000-1000-8000-00805f9b34fb",
                        "00001812-0000-1000-8000-00805f9b34fb"].contains(uuid.lowercased()) { return nil }
                    let chars = (service["characteristics"] as? [[String: Any]] ?? []).compactMap { item -> BluetoothCharacteristicDefinition? in
                        guard let key = item["key"] as? String, let uuid = item["uuid"] as? String else { return nil }
                        return BluetoothCharacteristicDefinition(key: key, uuid: uuid, readable: item["readable"] as? Bool ?? false,
                            writable: item["writable"] as? Bool ?? false, notifiable: item["notifiable"] as? Bool ?? false,
                            requiresEncryption: false, indicates: item["indicates"] as? Bool ?? false, writeWithoutResponse: item["writeWithoutResponse"] as? Bool ?? false,
                            writeWithResponse: item["writeWithResponse"] as? Bool, notifies: item["notifies"] as? Bool)
                    }
                    return BluetoothServiceDefinition(key: key, uuid: uuid, characteristics: chars)
                }
                return BluetoothPeripheralConfiguration(localName: localName, guestIdentity: identity, services: services, requestSeconds: requestSeconds)
            }
            guard ProcessInfo.processInfo.systemUptime < deadline else { throw BluetoothAdapterError.unavailable("Guest 蓝牙服务发现超时") }
            try await Task.sleep(for: .milliseconds(100))
        }
    }

    func openAdapterSession(_ session: UUID) {
        sessionID = session
        polling = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, self.sessionID == session else { return }
                do {
                    let status = try await self.call("external-status")
                    guard self.sessionID == session else { return }
                    if status["state"] as? String == "failed" { throw BluetoothAdapterError.unavailable(status["error"] as? String ?? "Guest 蓝牙已断开") }
                    self.onStatus?(status["error"] as? String)
                    for item in status["notifications"] as? [[String: Any]] ?? [] {
                        guard let key = item["key"] as? String, let hex = item["value"] as? String, let value = Self.data(hex) else { continue }
                        self.notificationHandler?(BluetoothGuestNotification(session: session, key: key, value: value))
                    }
                    try await Task.sleep(for: .milliseconds(20))
                } catch is CancellationError { return }
                catch { self.onFailure?(String(describing: error)); return }
            }
        }
    }

    private func operation(session: UUID, kind: String, key: String, value: Data = Data(), offset: Int = 0, enabled: Bool = false) async throws -> Data {
        guard sessionID == session else { throw BluetoothAdapterError.unavailable("蓝牙连接已关闭") }
        let submitted = try await call("external-request", ["operation": kind, "key": key, "value": value.map { String(format: "%02x", $0) }.joined(), "offset": offset, "enabled": enabled])
        guard let operation = submitted["operationID"] as? String else { throw BluetoothAdapterError.unavailable("Guest 未接收 ATT 请求") }
        let deadline = ProcessInfo.processInfo.systemUptime + requestSeconds
        while sessionID == session {
            try Task.checkCancellation()
            let result = try await call("external-result", ["operationID": operation])
            if result["done"] as? Bool == true {
                if let error = result["error"] as? String { throw BluetoothAdapterError.unavailable(error) }
                if let code = result["attError"] as? Int { throw BluetoothGuestATTError(code: code) }
                return Self.data(result["value"] as? String ?? "") ?? Data()
            }
            guard ProcessInfo.processInfo.systemUptime < deadline else { throw BluetoothAdapterError.unavailable("Guest ATT 请求超时") }
            try await Task.sleep(for: .milliseconds(10))
        }
        throw BluetoothAdapterError.unavailable("蓝牙连接已关闭")
    }

    func read(session: UUID, key: String, offset: Int, maximumLength: Int) async throws -> Data {
        let value = try await operation(session: session, kind: "read", key: key, offset: offset)
        return Data(value.prefix(maximumLength))
    }
    func write(session: UUID, key: String, value: Data) async throws { _ = try await operation(session: session, kind: "write", key: key, value: value) }
    func setNotifications(session: UUID, key: String, enabled: Bool) async throws { _ = try await operation(session: session, kind: "subscribe", key: key, enabled: enabled) }
    func reconnect(session: UUID) async throws {
        guard sessionID == session else { throw BluetoothAdapterError.unavailable("蓝牙连接已关闭") }
        _ = try await call("external-reconnect")
        let deadline = ProcessInfo.processInfo.systemUptime + requestSeconds
        while sessionID == session {
            try Task.checkCancellation()
            let status = try await call("external-status")
            if status["state"] as? String == "failed" {
                throw BluetoothAdapterError.unavailable(status["error"] as? String ?? "Guest 蓝牙重连失败")
            }
            if status["state"] as? String == "ready" { return }
            guard ProcessInfo.processInfo.systemUptime < deadline else {
                throw BluetoothAdapterError.unavailable("Guest 蓝牙重连超时")
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw BluetoothAdapterError.unavailable("蓝牙连接已关闭")
    }
    func close(session: UUID) { guard sessionID == session else { return }; shutdown() }

    func shutdown() {
        polling?.cancel(); polling = nil; sessionID = nil
        guard let identity = bridgeID else { return }
        bridgeID = nil
        let rpc = rpc
        Task { _ = try? await rpc(["schema": 1, "requestID": UUID().uuidString.lowercased(), "command": "external-close", "bridgeID": identity]) }
    }
    private static func data(_ hex: String) -> Data? {
        guard hex.count % 2 == 0 else { return nil }
        var value = Data(); var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            value.append(byte); index = next
        }
        return value
    }
}

/// One explicit radio owner per instance; construction never requests permission.
@MainActor final class BluetoothExternalSession: ObservableObject {
    // CoreBluetooth publishes into a shared host GATT database. Multiple guest
    // runtimes may run, but this app exposes one virtual peripheral at a time.
    private static weak var radioOwner: BluetoothExternalSession?
    @Published private(set) var isActive = false
    @Published private(set) var isStarting = false
    @Published private(set) var statusText = "未开启"
    @Published private(set) var lastError: String?
    private let rpc: BluetoothLiveGuestTransport.RPC
    private var transport: BluetoothLiveGuestTransport?
    private var adapter: BluetoothPeripheralAdapter?
    private var startTask: Task<Void, Never>?
    private var observation: Task<Void, Never>?
    private var generation = 0

    init(rpc: @escaping BluetoothLiveGuestTransport.RPC) { self.rpc = rpc }

    func start(localName: String, requestSeconds: TimeInterval = 30) {
        guard !isStarting, !isActive else { return }
        if let previous = Self.radioOwner, previous !== self { previous.stop() }
        Self.radioOwner = self
        generation += 1; let token = generation
        isStarting = true; lastError = nil; statusText = "正在读取设备蓝牙服务"
        let transport = BluetoothLiveGuestTransport(requestSeconds: requestSeconds, rpc: rpc)
        self.transport = transport
        transport.onFailure = { [weak self] error in self?.failed(error) }
        transport.onStatus = { [weak self] error in if let error { self?.lastError = error } }
        startTask = Task { [weak self] in
            do {
                let configuration = try await transport.prepare(localName: localName)
                guard let self, self.generation == token, !Task.isCancelled else { transport.shutdown(); return }
                let adapter = BluetoothPeripheralAdapter(guest: transport)
                self.adapter = adapter
                try adapter.startFromUserAction(configuration: configuration)
                self.isStarting = false; self.isActive = true
                self.observation = Task { [weak self] in
                    while !Task.isCancelled {
                        guard let self, self.generation == token else { return }
                        switch adapter.state {
                        case .advertising: self.statusText = "系统广播已启动：\(configuration.advertisingName)，等待外部连接"
                        case .waitingForPower:
                            self.statusText = CoreBluetoothPeripheralDriver.authorizationPending
                                ? "请在 macOS 弹窗中允许蓝牙访问" : "等待系统蓝牙就绪"
                        case .publishing: self.statusText = "正在发布设备蓝牙服务"
                        case .advertisingPending: self.statusText = "正在启动蓝牙广播：\(configuration.advertisingName)"
                        case .failed(let error): self.failed(error); return
                        case .stopped: return
                        }
                        do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
                    }
                }
            } catch is CancellationError { transport.shutdown() }
            catch { guard let self, self.generation == token else { return }; self.failed(String(describing: error)) }
        }
    }
    func stop() {
        generation += 1; startTask?.cancel(); startTask = nil; observation?.cancel(); observation = nil
        adapter?.stop(); adapter = nil; transport?.shutdown(); transport = nil
        isStarting = false; isActive = false; statusText = "未开启"
        if Self.radioOwner === self { Self.radioOwner = nil }
    }
    private func failed(_ error: String) { stop(); lastError = error; statusText = "蓝牙连接失败" }
}
