import Foundation
import Observation

@MainActor @Observable
final class SimulatorInstanceContext: Identifiable {
    let id: UUID
    var record: SimulatorInstanceRecord
    let runtime: StandaloneRuntimeModel
    let sharing = HostStatusSharingModel()
    let recording = SimulatorRecordingModel()
    let bluetooth: BluetoothExternalSession
    let guestOperations = SimulatorGuestOperationsModel()
    let logStream = SimulatorLogStreamModel()
    /// Set by Run in the menu/toolbar when the selected firmware still needs a fingerprint review.
    var runReview: SimulatorRunReview?

    init(record: SimulatorInstanceRecord, maximumRuntimeSeconds: Int = StandaloneRunLimits.unlimited) {
        self.id = record.id
        self.record = record
        runtime = StandaloneRuntimeModel(maximumRuntimeSeconds: maximumRuntimeSeconds, modelConstraint: record.model, instanceID: record.id)
        runtime.selectedProfileID = record.selectedProfileID
        runtime.virtualIdentity = record.virtualIdentity
        runtime.skipOOBE = record.skipOOBE ?? true
        let session = runtime.runtimeSession
        bluetooth = BluetoothExternalSession { [weak session] request in
            guard let session else { throw SimulatorError.disconnected }
            return try await session.externalBluetooth(request)
        }
    }
}

struct SimulatorRunReview: Identifiable {
    let id = UUID()
    let summary: String
    let runLimitSeconds: Int
}

/// Instance ownership is independent from selection: changing the sidebar never
/// stops another runtime or borrows another instance's session/IPC endpoints.
@MainActor @Observable
final class SimulatorInstanceLibrary {
    private(set) var instances: [SimulatorInstanceContext] = []
    /// The sidebar selection survives relaunches (a convenience, not state).
    var selectedID: UUID? {
        didSet { if oldValue != selectedID { UserDefaults.standard.set(selectedID?.uuidString, forKey: Self.selectionKey) } }
    }
    var errorMessage: String?
    private(set) var storageAvailable = true
    /// Set by the New menu command (⌘N); the manager view presents the sheet.
    var creationRequested = false
    var requestedIdentityID: UUID?
    var requestedResetID: UUID?
    var requestedDeleteID: UUID?
    let catalog: StandaloneRuntimeModel
    @ObservationIgnored private let store: SimulatorInstanceRecordStore
    @ObservationIgnored private let maximumRuntimeSeconds: Int
    @ObservationIgnored private var launchMaintenance: Task<Void, Never>?
    private static let selectionKey = "AzkiSimulator.selectedInstanceID"
    /// Upper bound for stopping one instance. The reaper, broker and storage
    /// commit are each bounded; this guards the sum and any unknown hang.
    static let stopTimeoutSeconds = 45.0

    init(store: SimulatorInstanceRecordStore = SimulatorInstanceRecordStore(),
         maximumRuntimeSeconds: Int = StandaloneRunLimits.unlimited,
         catalog: StandaloneRuntimeModel? = nil) {
        self.store = store
        self.maximumRuntimeSeconds = maximumRuntimeSeconds
        self.catalog = catalog ?? StandaloneRuntimeModel()
        loadInstances()
    }

    private func loadInstances() {
        do {
            instances = try store.load().map { SimulatorInstanceContext(record: $0, maximumRuntimeSeconds: maximumRuntimeSeconds) }
            storageAvailable = true
            let saved = UserDefaults.standard.string(forKey: Self.selectionKey).flatMap(UUID.init(uuidString:))
            selectedID = instances.contains { $0.id == saved } ? saved : instances.first?.id
        } catch {
            storageAvailable = false
            // Shown inline in the empty state with a retry, not as a modal.
            storageFailure = "无法读取设备实例：\(error.localizedDescription)"
        }
    }

    /// Why the instance list could not be read; nil when storage is usable.
    private(set) var storageFailure: String?

    /// Retry reading the instance list after a transient failure (e.g. a
    /// locked or briefly unavailable data folder). Never rewrites the file.
    func reloadInstances() {
        guard !storageAvailable else { return }
        storageFailure = nil
        loadInstances()
        if storageAvailable { beginLaunchMaintenance(force: true) }
    }

    /// Once per launch: remove stale session roots, then let every instance
    /// repair its own folder. Each step is non-fatal and scoped to its
    /// instance; a failure never blocks the list or other instances.
    func beginLaunchMaintenance(force: Bool = false) {
        guard launchMaintenance == nil || force else { return }
        if StandaloneRuntimeModel.launchCleanup == nil {
            let catalog = catalog
            StandaloneRuntimeModel.launchCleanup = Task { @MainActor in
                await catalog.cleanupStaleSessions()
                await catalog.autoStorageMaintenance()
            }
        }
        let targets = instances
        launchMaintenance = Task { @MainActor in
            // The library-wide pass settles each instance once; per-instance
            // resets must not race it for the same storage lock.
            await StandaloneRuntimeModel.waitForLaunchCleanup()
            await withTaskGroup(of: Void.self) { group in
                for instance in targets { group.addTask { @MainActor in await instance.runtime.resetAtLaunch() } }
            }
        }
    }

    var runningCount: Int { instances.filter { $0.runtime.isServing || $0.runtime.runtimeSession.isRunning }.count }
    var selected: SimulatorInstanceContext? { instances.first { $0.id == selectedID } }
    /// Hardware presets come from the built-in device metadata. Firmware is
    /// always the user's own import, so a preset never implies a version.
    var deviceModels: [BundledDevicePreset] {
        let tail: [String: Int] = ["miwear.watch.p65": 0, "miwear.watch.q65": 1]
        let sorted = catalog.devicePresets.sorted {
            let left = tail[$0.model]
            let right = tail[$1.model]
            if (left != nil) != (right != nil) { return left == nil }
            if let left, let right, left != right { return left < right }
            return $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending
        }
        // The Genshin edition belongs directly below the ceramic edition,
        // not wherever localized display-name order happens to place it.
        guard let genshin = sorted.first(where: { $0.model == "miwear.watch.q66ys" }) else { return sorted }
        var result = sorted.filter { $0.model != "miwear.watch.q66ys" }
        if let ceramicIndex = result.firstIndex(where: { $0.model == "miwear.watch.q66tc" }) {
            result.insert(genshin, at: result.index(after: ceramicIndex))
        } else {
            result.append(genshin)
        }
        return result
    }

    @discardableResult func create(model: String, name: String) -> SimulatorInstanceContext? {
        do { return try createNamed(model: model, name: name) }
        catch { errorMessage = "无法创建实例：\(error.localizedDescription)"; return nil }
    }

    /// Both naming operations validate/save a candidate document before changing
    /// observable objects. Failure leaves the sheet draft and original data intact.
    @discardableResult func createNamed(model: String, name: String, identity: SimulatorVirtualIdentity? = nil, skipOOBE: Bool = true) throws -> SimulatorInstanceContext {
        guard storageAvailable else { throw SimulatorInstanceStoreError.unsafe("实例存储不可用") }
        guard instances.count < SimulatorInstanceRecordStore.maximumRecords else {
            throw SimulatorInstanceStoreError.invalid("最多保存 128 个实例")
        }
        guard deviceModels.contains(where: { $0.model == model }) else {
            throw SimulatorInstanceStoreError.invalid("请选择目录中的设备型号")
        }
        let id = UUID()
        let identity = try (identity ?? .generated(for: id)).validated(model: model)
        let record = SimulatorInstanceRecord(id: id, name: name.trimmingCharacters(in: .whitespacesAndNewlines),
            model: model, selectedProfileID: nil, createdAt: Date(), virtualIdentity: identity, skipOOBE: skipOOBE)
        try store.save(instances.map(\.record) + [record])
        let instance = SimulatorInstanceContext(record: record, maximumRuntimeSeconds: maximumRuntimeSeconds)
        instances.append(instance)
        selectedID = instance.id
        return instance
    }

    func rename(_ id: UUID, name: String) throws {
        guard storageAvailable else { throw SimulatorInstanceStoreError.unsafe("实例存储不可用") }
        guard let index = instances.firstIndex(where: { $0.id == id }) else {
            throw SimulatorInstanceStoreError.invalid("实例不存在")
        }
        var candidate = instances.map(\.record)
        candidate[index].name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        try store.save(candidate)
        instances[index].record = candidate[index]
    }

    /// Delete one instance: stop it, drop its record, then remove its own data.
    /// Imported firmware belongs to the library and is never touched here.
    @discardableResult func delete(_ id: UUID) async -> Bool {
        guard storageAvailable else { errorMessage = "实例存储不可用"; return false }
        guard let instance = instances.first(where: { $0.id == id }) else { return false }
        // A starting, checking or running instance is stopped first; stop()
        // also cancels its in-flight helper commands.
        if instance.runtime.isBusy || instance.runtime.isServing || instance.runtime.runtimeSession.isAttached
            || instance.sharing.isSharing {
            guard await Self.stopInstance(instance) else {
                errorMessage = "未能确认「\(instance.record.name)」已停止，实例未删除。请稍后重试，或先对它使用「重置实例」。"
                return false
            }
        }
        // The record goes first: a failed cleanup must not leave a record that
        // points at data which is already gone.
        do { try store.save(instances.filter { $0.id != id }.map(\.record)) }
        catch { errorMessage = "无法删除实例：\(error.localizedDescription)"; return false }
        instances.removeAll { $0.id == id }
        if selectedID == id { selectedID = instances.first?.id }
        if !(await instance.runtime.discardInstanceData()) { errorMessage = instance.runtime.errorMessage }
        return true
    }

    /// Recovery for one instance: stop it and reset its runtime state. Errors
    /// stay on that instance; the list and other instances are unaffected.
    func resetInstance(_ id: UUID) async {
        guard let instance = instances.first(where: { $0.id == id }) else { return }
        instance.bluetooth.stop()
        await instance.recording.stop()
        _ = await instance.sharing.disable()
        _ = await SimulatorDeadline.bounded(Self.stopTimeoutSeconds + 30, fallback: false) {
            await instance.runtime.resetInstance()
        }
    }

    /// Stop one instance's external connections and core, bounded in time.
    static func stopInstance(_ instance: SimulatorInstanceContext) async -> Bool {
        await SimulatorDeadline.bounded(stopTimeoutSeconds, fallback: false) {
            instance.bluetooth.stop()
            await instance.recording.stop()
            guard await instance.sharing.disable() else { return false }
            let stopped = await instance.runtime.stop()
            if stopped { instance.runtime.runtimeSession.detach() }
            return stopped
        }
    }

    func defaultName(for model: String) -> String {
        let title = deviceModels.first(where: { $0.model == model }).map(Self.deviceName) ?? model
        return "\(title) \(instances.count + 1)"
    }

    func setVirtualIdentity(_ id: UUID, identity: SimulatorVirtualIdentity) throws {
        guard storageAvailable, let instance = instances.first(where: { $0.id == id }) else {
            throw SimulatorInstanceStoreError.invalid("实例存储不可用")
        }
        guard !instance.runtime.isBusy else { throw SimulatorInstanceStoreError.invalid("请等待设备启动或停止完成") }
        let identity = try identity.validated(model: instance.record.model)
        var candidate = instances.map(\.record)
        guard let index = candidate.firstIndex(where: { $0.id == id }) else { return }
        candidate[index].virtualIdentity = identity
        try store.save(candidate)
        instance.record = candidate[index]
        instance.runtime.virtualIdentity = identity
    }

    func persistSelection(_ instance: SimulatorInstanceContext) {
        guard storageAvailable else { return }
        let previous = instance.record
        instance.record.selectedProfileID = instance.runtime.selectedProfileID
        do { try store.save(instances.map(\.record)) }
        catch { instance.record = previous; errorMessage = "未能保存固件选择：\(error.localizedDescription)" }
    }

    func setSkipOOBE(_ id: UUID, enabled: Bool) throws {
        guard storageAvailable, let instance = instances.first(where: { $0.id == id }),
              !instance.runtime.isBusy, !instance.runtime.isServing, !instance.runtime.runtimeSession.isAttached else {
            throw SimulatorInstanceStoreError.invalid("停止设备后可修改首次设置选项")
        }
        var candidate = instances.map(\.record)
        guard let index = candidate.firstIndex(where: { $0.id == id }) else { return }
        candidate[index].skipOOBE = enabled
        try store.save(candidate)
        instance.record = candidate[index]
        instance.runtime.skipOOBE = enabled
    }

    /// Names of instances whose stop could not be confirmed (empty on success).
    /// Concurrent and individually bounded: a slow or stuck guest neither
    /// postpones its peers nor holds Quit forever.
    func stopAll() async -> [String] {
        await withTaskGroup(of: String?.self, returning: [String].self) { group in
            for instance in instances {
                group.addTask { @MainActor in await Self.stopInstance(instance) ? nil : instance.record.name }
            }
            var failed: [String] = []
            for await name in group { if let name { failed.append(name) } }
            return failed
        }
    }

    static func deviceName(_ preset: BundledDevicePreset) -> String {
        preset.displayName.components(separatedBy: " · ").first ?? preset.model
    }
}

/// An empty field always shows and uses the selected model's default name, so
/// clearing the text returns to following the model instead of freezing it.
nonisolated struct SimulatorInstanceNameDraft: Equatable {
    private(set) var text = ""
    private(set) var defaultName: String

    init(defaultName: String) { self.defaultName = defaultName }
    var name: String { text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? defaultName : text }
    mutating func edit(_ value: String) { text = value }
    mutating func updateDefault(_ value: String) { defaultName = value }
}
