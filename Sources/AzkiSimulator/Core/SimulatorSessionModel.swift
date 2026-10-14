import Foundation
import Observation
import CoreGraphics

@MainActor @Observable
final class SimulatorSessionModel {
    private(set) var manifest: SimulatorSessionManifest?
    private(set) var frame: SimulatorFrame?
    private(set) var image: CGImage?
    private(set) var status: SimulatorJSON?
    private(set) var logs = ""
    private(set) var packageReport = ""
    private(set) var screenshotName: String?
    private(set) var adcState: SimulatorJSON?
    private(set) var isConnecting = false
    private(set) var isCommandInFlight = false
    var errorMessage: String?

    @ObservationIgnored private var connection: SimulatorSessionConnection?
    @ObservationIgnored private var frameReader: SimulatorFrameReader?
    @ObservationIgnored private var polling: Task<Void, Never>?
    @ObservationIgnored private var statusPolling: Task<Void, Never>?
    @ObservationIgnored private var connecting: Task<Void, Never>?
    @ObservationIgnored private var inputWorker: Task<Void, Never>?
    @ObservationIgnored private var inputQueue: [SimulatorInputEvent] = []
    @ObservationIgnored private var generation = UUID()
    @ObservationIgnored private var statusRevision: UInt64 = 0

    var isAttached: Bool { connection != nil }
    var isRunning: Bool { status?["state"]?.string == "running" }
    var canStart: Bool { isAttached && !isRunning && status?["executionApproved"]?.bool == true && !isCommandInFlight }
    var canInput: Bool { isRunning && image != nil && !isCommandInFlight && manifest?.capabilities.contains("input") == true }
    var stateLabel: String {
        if isConnecting { return "连接中" }
        guard isAttached else { return "未连接" }
        switch status?["state"]?.string {
        case "running": return "运行中"
        case "stopped": return "已停止"
        case "crashed": return "已退出"
        default: return "已连接"
        }
    }

    func attach(_ url: URL) {
        detach()
        let token = generation
        isConnecting = true
        errorMessage = nil
        connecting = Task { [weak self] in
            do {
                let worker = Task.detached(priority: .userInitiated) { try SimulatorSessionConnection(directoryURL: url) }
                let candidate = try await worker.value
                guard let self, self.generation == token, !Task.isCancelled else { await candidate.close(); return }
                do {
                    let current: SimulatorJSON?
                    do { current = try await candidate.command("status") }
                    catch is SimulatorConnectionTimeout {
                        current = nil
                        self.errorMessage = SimulatorConnectionTimeout().localizedDescription
                    }
                    guard self.generation == token, !Task.isCancelled else { await candidate.close(); return }
                    let reader = try await candidate.makeFrameReader()
                    guard self.generation == token, !Task.isCancelled else {
                        await reader.close(); await candidate.close(); return
                    }
                    self.connection = candidate
                    self.frameReader = reader
                    self.manifest = candidate.manifest
                    self.status = current
                    self.isConnecting = false
                    self.beginPolling(reader, token: token)
                    self.beginStatusPolling(candidate, token: token)
                } catch {
                    await candidate.close()
                    throw error
                }
            } catch {
                guard let self, self.generation == token, !Task.isCancelled else { return }
                self.isConnecting = false
                self.handleConnectionFailure(error)
            }
        }
    }

    /// Detach only releases the UI's resources/contact. It never kills a guest
    /// the agent/user started independently; stopping is an explicit action.
    func detach() {
        generation = UUID()
        polling?.cancel(); polling = nil
        statusPolling?.cancel(); statusPolling = nil
        connecting?.cancel(); connecting = nil
        inputWorker?.cancel(); inputWorker = nil
        inputQueue.removeAll()
        if let connection { Task { await connection.close() } }
        if let frameReader { Task { await frameReader.close() } }
        connection = nil
        frameReader = nil
        manifest = nil; frame = nil; image = nil; status = nil
        logs = ""; packageReport = ""; screenshotName = nil; adcState = nil
        isConnecting = false; isCommandInFlight = false
    }

    func fetchLogs(_ name: String) {
        perform("logs", arguments: ["name": .string(name)])
    }

    func perform(_ command: String, arguments: [String: SimulatorJSON] = [:]) {
        guard let connection, !isCommandInFlight else { return }
        let token = generation
        isCommandInFlight = true
        statusRevision &+= 1
        if command == "stop" {
            inputQueue.removeAll()
            inputWorker?.cancel()
            inputWorker = nil
        }
        errorMessage = nil
        Task { [weak self] in
            do {
                let result = try await connection.command(command, arguments: arguments)
                guard let self, self.generation == token else { return }
                if command == "logs" { self.logs = result["text"]?.string ?? result.prettyDescription }
                if command == "screenshot" { self.screenshotName = result["file"]?.string }
                if command == "pmu-adc-get" || command == "pmu-adc-set" { self.adcState = result }
                let current = try await connection.command("status")
                guard self.generation == token else { return }
                if self.status != current { self.status = current }
                self.isCommandInFlight = false
            } catch {
                guard let self, self.generation == token else { return }
                self.isCommandInFlight = false
                self.handleConnectionFailure(error)
            }
        }
    }

    func inspectPackage(_ url: URL) {
        packageCommand(url, command: "inspect-package")
    }

    func stagePackage(_ url: URL) {
        packageCommand(url, command: "stage-package")
    }

    /// Guest operations return immediately with an operation identity. They do
    /// not hold the touch/UI command flag during the subsequent firmware work.
    func guestCommand(_ command: String, arguments: [String: SimulatorJSON] = [:]) async throws -> SimulatorJSON {
        guard let connection, isRunning else { throw SimulatorError.disconnected }
        guard ["guest-status", "guest-connect", "guest-operation", "guest-inventory", "guest-select"].contains(command) else {
            throw SimulatorError.remote("不受支持的设备操作")
        }
        let token = generation
        let result = try await connection.command(command, arguments: arguments)
        guard generation == token else { throw SimulatorError.disconnected }
        return result
    }

    func externalBluetooth(_ request: [String: Any]) async throws -> [String: Any] {
        guard let connection, isRunning else { throw SimulatorError.disconnected }
        guard let command = request["command"] as? String,
              ["external-open", "external-status", "external-request", "external-result", "external-close", "external-reconnect"].contains(command) else {
            throw SimulatorError.remote("不受支持的蓝牙操作")
        }
        var payload = request
        for key in ["schema", "requestID", "command"] { payload.removeValue(forKey: key) }
        let arguments = try JSONDecoder().decode([String: SimulatorJSON].self,
            from: JSONSerialization.data(withJSONObject: payload))
        let token = generation
        let result = try await connection.command(command, arguments: arguments)
        guard generation == token else { throw SimulatorError.disconnected }
        guard let object = try JSONSerialization.jsonObject(with: JSONEncoder().encode(result)) as? [String: Any] else {
            throw SimulatorError.remote("蓝牙操作响应无效")
        }
        return object
    }

    func installGuestPackage(_ url: URL, expectedSessionID: String, expectedGeneration: Int) async throws -> SimulatorJSON {
        guard let connection, isRunning else { throw SimulatorError.disconnected }
        let token = generation
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        let result = try await connection.installGuestPackage(url,
            expectedSessionID: expectedSessionID, expectedGeneration: expectedGeneration)
        guard generation == token else { throw SimulatorError.disconnected }
        return result
    }

    private func packageCommand(_ url: URL, command: String) {
        guard let connection, !isCommandInFlight, url.isFileURL else { return }
        let token = generation
        let access = url.startAccessingSecurityScopedResource()
        isCommandInFlight = true
        errorMessage = nil
        Task { [weak self] in
            defer { if access { url.stopAccessingSecurityScopedResource() } }
            do {
                let result = try await connection.command(command, arguments: ["path": .string(url.path)])
                guard let self, self.generation == token else { return }
                self.packageReport = result.prettyDescription
                self.isCommandInFlight = false
            } catch {
                guard let self, self.generation == token else { return }
                self.isCommandInFlight = false
                self.handleConnectionFailure(error)
            }
        }
    }

    /// Coalesce only consecutive movement while preserving every down/up edge.
    /// This bounds work at high mouse rates and preserves the final release.
    func sendInput(_ event: SimulatorInputEvent) {
        guard let connection, canInput || !event.down else { return }
        if event.down, inputQueue.count >= 2, inputQueue.last?.down == true, inputQueue[inputQueue.count - 2].down {
            inputQueue[inputQueue.count - 1] = event
        }
        else { inputQueue.append(event) }
        guard inputQueue.count <= 128 else {
            detach()
            errorMessage = "输入队列已满，会话已安全断开"
            return
        }
        guard inputWorker == nil else { return }
        let token = generation
        inputWorker = Task { [weak self] in
            while let self, self.generation == token, !Task.isCancelled, !self.inputQueue.isEmpty {
                let event = self.inputQueue.removeFirst()
                do { try await connection.input(event) }
                catch {
                    guard self.generation == token, !Task.isCancelled else { return }
                    self.inputQueue.removeAll()
                    self.handleConnectionFailure(error)
                    break
                }
            }
            guard let self, self.generation == token, !Task.isCancelled else { return }
            self.inputWorker = nil
        }
    }

    private func beginPolling(_ reader: SimulatorFrameReader, token: UUID) {
        polling = Task { [weak self] in
            var sequence: UInt32?
            let clock = ContinuousClock()
            var cadence = SimulatorFrameCadence(now: clock.now)
            var failureSince: ContinuousClock.Instant?
            while !Task.isCancelled {
                guard let self, self.generation == token else { return }
                do {
                    if let next = try await reader.presentation(after: sequence) {
                        guard self.generation == token, !Task.isCancelled else { return }
                        self.frame = next.frame
                        self.image = next.image
                        sequence = next.frame.sequence
                    }
                    failureSince = nil
                } catch SimulatorError.frameBeingWritten {
                    // An in-progress seqlock is retried at the next deadline.
                } catch {
                    guard self.generation == token, !Task.isCancelled else { return }
                    if error as? SimulatorError == .disconnected {
                        self.handleConnectionFailure(error)
                        return
                    }
                    if failureSince == nil { failureSince = clock.now }
                    if self.isRunning, let since = failureSince, since.duration(to: clock.now) >= .seconds(1) {
                        self.errorMessage = error.localizedDescription
                        failureSince = clock.now
                    }
                }
                try? await clock.sleep(until: cadence.next(after: clock.now), tolerance: .milliseconds(1))
            }
        }
    }

    private func beginStatusPolling(_ connection: SimulatorSessionConnection, token: UUID) {
        statusPolling = Task { [weak self] in
            while !Task.isCancelled {
                // Initial status was already obtained by attach. Slow status or
                // log commands never sit on the display's executor/deadline.
                try? await Task.sleep(for: .seconds(1))
                guard let self, self.generation == token, !Task.isCancelled else { return }
                let revision = self.statusRevision
                do {
                    let current = try await connection.command("status")
                    guard self.generation == token, !Task.isCancelled else { return }
                    if revision == self.statusRevision, self.status != current { self.status = current }
                } catch {
                    guard self.generation == token, !Task.isCancelled else { return }
                    self.handleConnectionFailure(error, disconnectOnFailure: true)
                    if !(error is SimulatorConnectionTimeout) { return }
                }
            }
        }
    }

    private func handleConnectionFailure(_ error: Error, disconnectOnFailure: Bool = false) {
        if error is SimulatorConnectionTimeout {
            // A slow control reply says nothing about guest liveness. Keep the
            // independent frame reader and let status polling recover.
            errorMessage = error.localizedDescription
        } else if error as? SimulatorError == .disconnected {
            detach()
            errorMessage = nil
        } else {
            if disconnectOnFailure { detach() }
            errorMessage = error.localizedDescription
        }
    }
}
