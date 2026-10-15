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

/// Instance ownership is independent from selection: changing the sidebar never
/// stops another runtime or borrows another instance's session/IPC endpoints.
@MainActor @Observable
final class SimulatorInstanceLibrary {
    private(set) var instances: [SimulatorInstanceContext] = []
    var selectedID: UUID?
    var errorMessage: String?
    private(set) var storageAvailable = true
    let catalog: StandaloneRuntimeModel
    @ObservationIgnored private let store: SimulatorInstanceRecordStore
    @ObservationIgnored private let maximumRuntimeSeconds: Int

    init(store: SimulatorInstanceRecordStore = SimulatorInstanceRecordStore(),
         maximumRuntimeSeconds: Int = StandaloneRunLimits.unlimited,
         catalog: StandaloneRuntimeModel? = nil) {
        self.store = store
        self.maximumRuntimeSeconds = maximumRuntimeSeconds
        self.catalog = catalog ?? StandaloneRuntimeModel()
        do {
            instances = try store.load().map { SimulatorInstanceContext(record: $0, maximumRuntimeSeconds: maximumRuntimeSeconds) }
            selectedID = instances.first?.id
        } catch {
            storageAvailable = false
            errorMessage = "无法读取设备实例：\(error.localizedDescription)"
        }
    }

    var runningCount: Int { instances.filter { $0.runtime.isServing || $0.runtime.runtimeSession.isRunning }.count }
    var selected: SimulatorInstanceContext? { instances.first { $0.id == selectedID } }
    var deviceModels: [BundledFirmwareItem] {
        var seen = Set<String>()
        return catalog.firmwareItems.filter { seen.insert($0.model).inserted }
            .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
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

    func stopAll() async -> Bool {
        // Concurrent cleanup: a slow guest must not postpone stopping its peers.
        let values = await withTaskGroup(of: Bool.self, returning: [Bool].self) { group in
            for instance in instances {
                group.addTask { @MainActor in
                    instance.bluetooth.stop()
                    await instance.recording.stop()
                    guard await instance.sharing.disable() else { return false }
                    let stopped = await instance.runtime.stop()
                    if stopped { instance.runtime.runtimeSession.detach() }
                    return stopped
                }
            }
            var values: [Bool] = []
            for await value in group { values.append(value) }
            return values
        }
        return values.allSatisfy { $0 }
    }

    static func deviceName(_ item: BundledFirmwareItem) -> String {
        item.displayName.components(separatedBy: " · ").first ?? item.model
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
