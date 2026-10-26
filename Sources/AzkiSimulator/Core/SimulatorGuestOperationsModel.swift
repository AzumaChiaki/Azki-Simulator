import Foundation
import Observation

/// One workspace owns one model. Switching the selected instance does not
/// cancel its operation; replacing/stopping its host session invalidates it.
@Observable @MainActor
final class SimulatorGuestOperationsModel {
    typealias Command = @MainActor (String, [String: SimulatorJSON]) async throws -> SimulatorJSON
    typealias Install = @MainActor (URL, String, Int) async throws -> SimulatorJSON

    struct Identity: Equatable {
        let sessionID: String
        let generation: Int
    }
    struct Watchface: Identifiable, Equatable {
        let id: String
        let name: String
        var isCurrent: Bool
    }
    enum PackageKind: String, CaseIterable { case watchface, quickapp }
    struct QuickApp: Identifiable, Equatable {
        let id: String
        let name: String
        let version: Int
        let fingerprint: String
    }

    private(set) var isAvailable = false
    private(set) var packagesAvailable = false
    private(set) var isAuthenticated = false
    private(set) var isTransportReady = false
    private(set) var authPhase = "disconnected"
    private(set) var identity: Identity?
    private(set) var operationID: String?
    private(set) var operationKind: String?
    private(set) var operationPackageKind: PackageKind?
    private(set) var operationPackageID: String?
    private(set) var operationState: String?
    private(set) var operationPhase: String?
    private(set) var acknowledgedSegments = 0
    private(set) var inventory: [Watchface] = []
    private(set) var inventoryLoaded = false
    private(set) var applications: [QuickApp] = []
    private(set) var applicationsLoaded = false
    private(set) var errorMessage: String?
    private(set) var resultMessage: String?
    private(set) var isRequestInFlight = false
    private(set) var isResolvingCompletion = false
    var selectedWatchfaceID: String?

    @ObservationIgnored private var hostSessionID: UUID?
    @ObservationIgnored private var peerProfileID: String?
    @ObservationIgnored private var revision = UUID()
    @ObservationIgnored private var pollRevision = UUID()
    @ObservationIgnored private var command: Command?
    @ObservationIgnored private var installPackage: Install?
    @ObservationIgnored private var isRefreshing = false
    @ObservationIgnored private var completedOperationID: String?
    @ObservationIgnored private var inventoryAttempted: Set<PackageKind> = []
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var pollGeneration = 0

    var isBusy: Bool { isRequestInFlight || isResolvingCompletion || operationState == "running" }
    var canConnect: Bool {
        isAvailable && identity != nil && !isBusy && !isAuthenticated
            && ["idle", "disconnected", "failed"].contains(authPhase)
    }
    // A package/request error does not invalidate authentication. The engine's
    // reported transport state determines whether another operation may start.
    var canInstall: Bool { isAvailable && packagesAvailable && isAuthenticated && isTransportReady && identity != nil && !isBusy }
    var canRefresh: Bool { canInstall }
    var canSelect: Bool {
        canInstall && selectedWatchfaceID.map { id in inventory.contains { $0.id == id && !$0.isCurrent } } == true
    }
    func canUninstall(_ app: QuickApp) -> Bool { canInstall && applications.contains(app) }
    var connectionLabel: String {
        if !isAvailable { return "当前设备不支持" }
        if isAuthenticated { return isTransportReady ? "本地通信已认证" : "本地通信未就绪" }
        switch authPhase {
        case "disconnected", "idle": return "未连接"
        case "failed", "error": return "连接失败"
        default: return "正在连接 · 请在设备画面确认"
        }
    }
    var progressLabel: String? {
        guard operationState == "running" else { return nil }
        switch operationKind {
        case "inventory": return operationPackageKind == .quickapp ? "正在读取应用" : "正在读取表盘"
        case "watchface-select": return "正在使用表盘"
        case "quickapp-uninstall":
            return operationPhase == "after-uninstall-inventory" ? "正在核对设备应用" : "正在卸载应用"
        case "install":
            if acknowledgedSegments > 0 { return "正在安装 · 已确认 \(acknowledgedSegments) 段" }
            switch operationPhase {
            case "wait_after": return "正在核对设备库存"
            case "wait_terminal": return "等待设备安装结果"
            default: return operationPackageKind == .quickapp ? "正在安装应用" : "正在安装"
            }
        default: return "正在处理"
        }
    }

    func configure(hostSessionID: UUID?, available: Bool, packagesAvailable: Bool = true, command: @escaping Command, install: @escaping Install) {
        let enabled = available && hostSessionID != nil
        if self.hostSessionID != hostSessionID || isAvailable != enabled {
            invalidate()
            self.hostSessionID = hostSessionID
            isAvailable = enabled
        }
        self.command = command
        self.packagesAvailable = packagesAvailable
        installPackage = install
        if enabled { ensurePolling() } else { stopPolling() }
    }

    private func ensurePolling() {
        guard pollTask == nil else { return }
        let generation = pollGeneration
        pollTask = Task { @MainActor [weak self] in
            while let self, !Task.isCancelled, self.isAvailable, self.pollGeneration == generation {
                await self.refresh()
                do { try await Task.sleep(for: .milliseconds(750)) }
                catch { break }
            }
            guard let self, self.pollGeneration == generation else { return }
            self.pollTask = nil
        }
    }

    private func stopPolling() {
        pollGeneration += 1
        pollTask?.cancel()
        pollTask = nil
    }

    func invalidate() {
        stopPolling()
        revision = UUID()
        hostSessionID = nil
        peerProfileID = nil
        isAvailable = false
        packagesAvailable = false
        isAuthenticated = false
        isTransportReady = false
        authPhase = "disconnected"
        identity = nil
        clearOperation()
        inventory = []; inventoryLoaded = false; inventoryAttempted = []
        applications = []; applicationsLoaded = false
        selectedWatchfaceID = nil
        errorMessage = nil; resultMessage = nil
        isRequestInFlight = false; isRefreshing = false; isResolvingCompletion = false
        command = nil; installPackage = nil
    }

    func refresh() async {
        guard isAvailable, !isRefreshing, !isRequestInFlight, let command else { return }
        let token = revision, readToken = pollRevision
        isRefreshing = true
        defer { if token == revision { isRefreshing = false; isResolvingCompletion = false } }
        do {
            let status = try await command("guest-status", [:])
            guard token == revision, readToken == pollRevision, !Task.isCancelled else { return }
            try applyStatus(status)
            if let id = operationID, operationState != "running", completedOperationID != id {
                let boundIdentity = identity
                guard let boundIdentity else { throw Self.invalid("设备连接已变化") }
                var arguments = Self.arguments(boundIdentity)
                arguments["operationID"] = .string(id)
                isResolvingCompletion = true
                let response = try await command("guest-operation", arguments)
                guard token == revision, readToken == pollRevision, identity == boundIdentity, !Task.isCancelled else { return }
                try applyCompletion(response, operationID: id)
                completedOperationID = id
                isResolvingCompletion = false
            }
            if canRefresh, let kind = PackageKind.allCases.first(where: { !inventoryAttempted.contains($0) }) {
                await loadInventory(kind: kind)
            }
        } catch {
            guard token == revision, readToken == pollRevision, !Task.isCancelled else { return }
            errorMessage = error.localizedDescription
        }
    }

    func connect() async {
        guard canConnect, let identity, let command else { return }
        let token = revision
        pollRevision = UUID()
        isRequestInFlight = true; errorMessage = nil; resultMessage = nil
        defer { if token == revision { isRequestInFlight = false } }
        do {
            let response = try await command("guest-connect", Self.arguments(identity))
            guard token == revision else { return }
            try applyStatus(response)
        } catch {
            guard token == revision else { return }
            errorMessage = error.localizedDescription
        }
    }

    func install(_ url: URL) async {
        guard canInstall, url.isFileURL, let identity, let installPackage else { return }
        await begin(kind: "install") {
            try await installPackage(url, identity.sessionID, identity.generation)
        }
    }

    func loadInventory(kind: PackageKind = .watchface) async {
        guard canRefresh, let identity, let command else { return }
        inventoryAttempted.insert(kind)
        await begin(kind: "inventory", packageKind: kind) {
            var arguments = Self.arguments(identity)
            arguments["kind"] = .string(kind.rawValue)
            return try await command("guest-inventory", arguments)
        }
    }

    func selectWatchface() async {
        guard canSelect, let id = selectedWatchfaceID, let identity, let command else { return }
        await begin(kind: "watchface-select", packageKind: .watchface) {
            var arguments = Self.arguments(identity)
            arguments["identifier"] = .string(id)
            return try await command("guest-select", arguments)
        }
    }

    func uninstall(_ app: QuickApp) async {
        guard canUninstall(app), let identity, let command else { return }
        await begin(kind: "quickapp-uninstall", packageKind: .quickapp, packageID: app.id) {
            var arguments = Self.arguments(identity)
            arguments["identifier"] = .string(app.id)
            arguments["fingerprint"] = .string(app.fingerprint)
            return try await command("guest-uninstall", arguments)
        }
    }

    private func begin(kind: String, packageKind: PackageKind? = nil, packageID: String? = nil, request: () async throws -> SimulatorJSON) async {
        let token = revision, boundIdentity = identity
        pollRevision = UUID()
        isRequestInFlight = true; errorMessage = nil; resultMessage = nil
        defer { if token == revision { isRequestInFlight = false } }
        do {
            let response = try await request()
            guard token == revision, identity == boundIdentity else { return }
            guard let id = response["operationID"]?.string, UUID(uuidString: id) != nil else {
                throw Self.invalid("设备未返回有效操作编号")
            }
            operationID = id; operationKind = kind; operationState = "running"
            operationPackageKind = packageKind; operationPackageID = packageID
            operationPhase = nil; acknowledgedSegments = 0; completedOperationID = nil
        } catch {
            guard token == revision else { return }
            errorMessage = error.localizedDescription
        }
    }

    private func applyStatus(_ status: SimulatorJSON) throws {
        guard isAvailable, hostSessionID != nil,
              let profile = status["profileID"]?.string, !profile.isEmpty, Self.validText(profile, limit: 128),
              peerProfileID == nil || peerProfileID == profile, status["backendKind"]?.string == "guest",
              let id = status["sessionID"]?.string, !id.isEmpty, id.utf8.count <= 128,
              let generation = Self.integer(status["generation"]) else {
            isAuthenticated = false
            isTransportReady = false
            throw Self.invalid("设备通信状态无效或固件不支持")
        }
        peerProfileID = profile
        let current = Identity(sessionID: id, generation: generation)
        if identity != current {
            identity = current
            clearOperation()
            inventory = []; inventoryLoaded = false; inventoryAttempted = []
            applications = []; applicationsLoaded = false
            selectedWatchfaceID = nil; errorMessage = nil; resultMessage = nil
        }
        isAuthenticated = status["authenticated"]?.bool == true
        isTransportReady = status["transportReady"]?.bool == true
        authPhase = status["authPhase"]?.string ?? (isAuthenticated ? "ready" : "disconnected")
        if let failure = status["idleError"]?.string, !failure.isEmpty { errorMessage = String(failure.prefix(512)) }
        if let operation = status["operation"], operation != .null {
            guard let id = operation["operationID"]?.string, UUID(uuidString: id) != nil,
                  let kind = operation["kind"]?.string, ["install", "inventory", "watchface-select", "quickapp-uninstall"].contains(kind),
                  let state = operation["state"]?.string, ["running", "completed", "failed"].contains(state) else {
                throw Self.invalid("设备操作状态无效")
            }
            operationID = id; operationKind = kind; operationState = state
            operationPackageKind = try Self.packageKind(operation, field: kind == "inventory" ? "inventoryKind" : "packageKind")
            operationPackageID = operation["packageID"]?.string
            operationPhase = operation["phase"]?.string
            acknowledgedSegments = Self.integer(operation["acknowledgedSegments"]) ?? 0
            if state == "failed" { errorMessage = operation["error"]?.string ?? "设备操作失败" }
        }
    }

    private func applyCompletion(_ response: SimulatorJSON, operationID id: String) throws {
        guard response["operationID"]?.string == id,
              response["kind"]?.string == operationKind, response["state"]?.string == operationState,
              let receipt = response["receipt"], receipt["operationID"]?.string == id,
              receipt["backendKind"]?.string == "guest" else { throw Self.invalid("设备操作结果未通过核对") }
        guard operationState == "completed", receipt["state"]?.string == "completed" else {
            errorMessage = receipt["error"]?.string ?? errorMessage ?? "设备操作失败"
            return
        }
        switch operationKind {
        case "install":
            let kind = try confirmedPackageKind(response, receipt: receipt, field: "packageKind")
            guard receipt["guestInstallerAccepted"]?.bool == true else { throw Self.invalid("设备尚未确认安装") }
            resultMessage = kind == .quickapp ? "应用已安装" : "表盘已安装"
            // The standard guest installer terminal is authoritative. Inventory
            // is optional display data, not an additional installation gate.
            if let entries = receipt["afterInventory"], entries != .null {
                do { try applyInventory(entries, kind: kind) }
                catch { errorMessage = "库存读取失败：\(error.localizedDescription)" }
            }
        case "watchface-select":
            guard try confirmedPackageKind(response, receipt: receipt, field: "packageKind") == .watchface,
                  receipt["guestSelectionVerified"]?.bool == true,
                  let target = receipt["targetInventory"], let id = target["id"]?.string,
                  target["current"]?.bool == true, inventory.contains(where: { $0.id == id }) else {
                throw Self.invalid("设备尚未确认使用此表盘")
            }
            inventory = inventory.map { .init(id: $0.id, name: $0.name, isCurrent: $0.id == id) }
            selectedWatchfaceID = id
            resultMessage = "设备已确认使用表盘"
        case "inventory":
            let kind = try confirmedPackageKind(response, receipt: receipt, field: "inventoryKind")
            guard receipt["guestInventoryVerified"]?.bool == true, receipt["readOnly"]?.bool == true else {
                throw Self.invalid("设备库存结果未通过核对")
            }
            if kind == .quickapp, receipt["currentID"] != .null { throw Self.invalid("设备应用库存状态无效") }
            try applyInventory(receipt["inventory"], kind: kind)
        case "quickapp-uninstall":
            guard try confirmedPackageKind(response, receipt: receipt, field: "packageKind") == .quickapp,
                  receipt["guestUninstallVerified"]?.bool == true,
                  let packageID = operationPackageID,
                  response["packageID"]?.string == packageID, receipt["packageID"]?.string == packageID,
                  case .array(let entries) = receipt["afterInventory"],
                  !entries.contains(where: { $0["id"]?.string == packageID }) else {
                throw Self.invalid("设备尚未确认卸载此应用")
            }
            try applyInventory(receipt["afterInventory"], kind: .quickapp)
            resultMessage = "应用已卸载"
        default: throw Self.invalid("设备操作类型无效")
        }
    }

    private func applyInventory(_ value: SimulatorJSON?, kind: PackageKind) throws {
        guard case .array(let entries) = value, entries.count <= 256 else { throw Self.invalid("设备库存无效") }
        var ids = Set<String>()
        if kind == .quickapp {
            let apps = try entries.map { entry -> QuickApp in
                guard let id = entry["id"]?.string, Self.validText(id, limit: 128), !id.isEmpty,
                      ids.insert(id).inserted, let name = entry["name"]?.string, Self.validText(name, limit: 512),
                      let version = Self.integer(entry["version"]), let fingerprint = entry["fingerprint"]?.string,
                      fingerprint.utf8.count <= 128,
                      fingerprint.range(of: "^(?:[0-9a-f]{2})*$", options: .regularExpression) != nil else {
                    throw Self.invalid("设备应用条目无效")
                }
                return QuickApp(id: id, name: name, version: version, fingerprint: fingerprint)
            }
            applications = apps; applicationsLoaded = true
            return
        }
        let faces = try entries.map { entry -> Watchface in
            guard let id = entry["id"]?.string, Self.validWatchfaceID(id), ids.insert(id).inserted,
                  let current = entry["current"]?.bool else { throw Self.invalid("设备表盘条目无效") }
            return Watchface(id: id, name: String((entry["name"]?.string ?? id).prefix(128)), isCurrent: current)
        }
        guard faces.filter(\.isCurrent).count <= 1 else { throw Self.invalid("设备返回了多个当前表盘") }
        inventory = faces; inventoryLoaded = true
        if !faces.contains(where: { $0.id == selectedWatchfaceID }) {
            selectedWatchfaceID = faces.first(where: \.isCurrent)?.id ?? faces.first?.id
        }
    }

    private func clearOperation() {
        operationID = nil; operationKind = nil; operationState = nil; operationPhase = nil
        operationPackageKind = nil; operationPackageID = nil
        acknowledgedSegments = 0; completedOperationID = nil
    }
    private static func arguments(_ identity: Identity) -> [String: SimulatorJSON] {
        ["expected_session_id": .string(identity.sessionID), "expected_generation": .number(Double(identity.generation))]
    }
    private static func integer(_ value: SimulatorJSON?) -> Int? {
        guard case .number(let number) = value, number.isFinite, number.rounded() == number,
              (0...9_007_199_254_740_991).contains(number) else { return nil }
        return Int(number)
    }
    private static func validWatchfaceID(_ id: String) -> Bool {
        id.range(of: "^(?:[0-9]{9,12}|L[0-9]{11})$", options: .regularExpression) != nil
    }
    private func confirmedPackageKind(_ operation: SimulatorJSON, receipt: SimulatorJSON, field: String) throws -> PackageKind {
        let kind = try Self.packageKind(operation, field: field)
        guard kind == operationPackageKind, try Self.packageKind(receipt, field: field) == kind else {
            throw Self.invalid("设备操作与库存类型不匹配")
        }
        return kind
    }
    private static func packageKind(_ value: SimulatorJSON, field: String) throws -> PackageKind {
        // Older verified watchface peers omitted these additive type fields.
        guard let raw = value[field] else { return .watchface }
        guard let name = raw.string, let kind = PackageKind(rawValue: name) else { throw invalid("设备包类型无效") }
        return kind
    }
    private static func validText(_ value: String, limit: Int) -> Bool {
        value.utf8.count <= limit && !value.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }
    private static func invalid(_ message: String) -> SimulatorError { .remote(message) }
}
