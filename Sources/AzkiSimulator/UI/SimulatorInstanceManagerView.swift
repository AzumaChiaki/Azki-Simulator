import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor struct SimulatorInstanceManagerView: View {
    @Bindable var library: SimulatorInstanceLibrary
    @Environment(\.openWindow) private var openWindow
    @State private var creating = false
    @State private var renaming: SimulatorInstanceContext?
    @State private var editingIdentity: SimulatorInstanceContext?
    @State private var deleting: SimulatorInstanceContext?
    @State private var resetting: SimulatorInstanceContext?
    @State private var inspectorPresented = true
    @State private var columnVisibility: NavigationSplitViewVisibility = .all
    @State private var logsPresented = false
    @State private var importFirmware = false

    var body: some View {
        NavigationSplitView(columnVisibility: $columnVisibility) {
            sidebar
                .navigationSplitViewColumnWidth(min: 180, ideal: 220, max: 280)
        } detail: {
            detail
        }
        .navigationSplitViewStyle(.balanced)
        .modifier(SimulatorInspectorAttachment(library: library, presented: $inspectorPresented))
        .background {
            ForEach(library.instances) { instance in
                SimulatorInstanceRuntimeHost(library: library, instance: instance)
            }
        }
        .focusedSceneValue(\.simulatorLibrary, library)
        .focusedSceneValue(\.simulatorInstance, library.selected)
        .focusedSceneValue(\.simulatorInspectorPresented, $inspectorPresented)
        .focusedSceneValue(\.simulatorLogsPresented, $logsPresented)
        .focusedSceneValue(\.simulatorImportFirmware, $importFirmware)
        .modifier(SimulatorScenePresentations(
            library: library, sceneInstance: nil, importFirmware: $importFirmware,
            creating: $creating, editingIdentity: $editingIdentity, deleting: $deleting, resetting: $resetting
        ))
        .task {
            library.beginLaunchMaintenance()
            await library.catalog.loadCatalog()
        }
        .onChange(of: library.selectedID) { _, id in
            if id == nil { columnVisibility = .all }
        }
        .sheet(item: $renaming) { SimulatorInstanceRenameSheet(library: library, instance: $0) }
        .toolbar {
            SimulatorInstanceActionToolbar(
                library: library, instance: library.selected,
                onCreate: { creating = true }, onImport: { importFirmware = true }
            )
        }
    }

    private var sidebar: some View {
        List(selection: $library.selectedID) {
            ForEach(library.instances) { instance in
                sidebarRow(instance)
                    .tag(instance.id)
                    .contextMenu { instanceMenu(instance) }
                    .accessibilityIdentifier("simulator.instance.\(instance.id.uuidString)")
            }
        }
        .listStyle(.sidebar)
        .navigationTitle("设备实例")
        .navigationSubtitle("\(library.instances.count) 台设备 · \(library.runningCount) 台运行中")
    }

    private func sidebarRow(_ instance: SimulatorInstanceContext) -> some View {
        Label {
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(instance.record.name).lineLimit(2)
                    if instance.runtime.isBusy {
                        ProgressView().controlSize(.mini)
                            .help(instance.runtime.isStopping ? "正在停止" : "正在处理")
                    } else if Self.hasError(instance) {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                            .help("此实例有未处理的错误；选中查看，或使用「重置实例」")
                            .accessibilityLabel("有错误")
                    }
                }
                Text(deviceTitle(instance)).foregroundStyle(.secondary).font(.caption)
                Text(instance.runtime.isStopping ? "正在停止" : instance.runtime.runtimeSession.stateLabel)
                    .foregroundStyle(.secondary).font(.caption)
            }
        } icon: {
            Image(systemName: instance.runtime.isServing ? "play.circle.fill" : "applewatch")
                .foregroundStyle(instance.runtime.isServing ? Color.green : Color.secondary)
        }
    }

    @ViewBuilder private func instanceMenu(_ instance: SimulatorInstanceContext) -> some View {
        Button("重命名…") { renaming = instance }
            .accessibilityIdentifier("simulator.rename-instance.\(instance.id.uuidString)")
        Button("虚拟设备身份…") { editingIdentity = instance }
        Button("在新窗口打开画面") { openWindow(id: "device-display", value: instance.id) }
        Button("重置实例…") { resetting = instance }
        Divider()
        Button("删除实例…", role: .destructive) { deleting = instance }
            .accessibilityIdentifier("simulator.delete-instance.\(instance.id.uuidString)")
    }

    @ViewBuilder private var detail: some View {
        if let instance = library.selected {
            SimulatorDeviceDetailView(instance: instance, logsPresented: $logsPresented)
                .navigationTitle(instance.record.name)
                .navigationSubtitle(instance.runtime.runtimeSession.stateLabel)
        } else if let failure = library.storageFailure {
            ContentUnavailableView {
                Label("无法读取设备实例", systemImage: "exclamationmark.triangle")
            } description: {
                Text(failure + "\n原有记录不会被改写；修复后点击重试。")
            } actions: {
                Button("重试") { library.reloadInstances() }
                Button("在访达中显示") {
                    NSWorkspace.shared.activateFileViewerSelecting([SimulatorInstanceRecordStore.defaultRootURL])
                }
            }
        } else if library.instances.isEmpty {
            ContentUnavailableView {
                Label("创建你的第一台设备", systemImage: "applewatch")
            } description: {
                if !library.catalog.hasLoadedCatalog || library.catalog.isBusy {
                    Text("正在读取设备型号…")
                } else if library.deviceModels.isEmpty {
                    Text(library.catalog.libraryStatus)
                } else {
                    Text("选择硬件型号即可创建；固件在实例里导入。")
                }
            } actions: {
                Button("新建设备…") { creating = true }
                    .disabled(SimulatorInstanceActions.canCreateReason(library) != nil)
                    .accessibilityIdentifier("simulator.create-instance.empty")
                if library.catalog.hasLoadedCatalog && !library.catalog.isBusy && library.deviceModels.isEmpty {
                    Button("重新读取固件库") { Task { await library.catalog.loadCatalog() } }
                }
            }
        } else {
            ContentUnavailableView("选择设备实例", systemImage: "sidebar.left")
        }
    }

    private func deviceTitle(_ instance: SimulatorInstanceContext) -> String {
        library.deviceModels.first { $0.model == instance.record.model }.map(SimulatorInstanceLibrary.deviceName)
            ?? instance.record.model
    }

    private static func hasError(_ instance: SimulatorInstanceContext) -> Bool {
        instance.runtime.errorMessage != nil || instance.runtime.runtimeSession.errorMessage != nil
            || instance.sharing.errorMessage != nil
    }
}

private struct SimulatorScenePresentations: ViewModifier {
    var library: SimulatorInstanceLibrary
    var sceneInstance: SimulatorInstanceContext?
    @Binding var importFirmware: Bool
    @Binding var creating: Bool
    @Binding var editingIdentity: SimulatorInstanceContext?
    @Binding var deleting: SimulatorInstanceContext?
    @Binding var resetting: SimulatorInstanceContext?
    @Environment(\.controlActiveState) private var controlActiveState

    private var isKey: Bool { controlActiveState == .key }
    private var presentedInstance: SimulatorInstanceContext? { sceneInstance ?? library.selected }

    // Split into separately type-checked stages: as one modifier chain the
    // older Swift 6.2 toolchain used by CI gives up type-checking it.
    func body(content: Content) -> some View {
        alerts(dialogs(requests(content)))
    }

    private func requests(_ content: some View) -> some View {
        content
            .fileImporter(isPresented: $importFirmware, allowedContentTypes: [.item]) { result in
                guard let instance = presentedInstance else { return }
                switch result {
                case .success(let url):
                    guard SimulatorInstanceActions.importFirmware(instance) else {
                        instance.runtime.errorMessage = "设备状态已变化，请先停止设备再导入固件。"
                        return
                    }
                    Task { await instance.runtime.importFirmware(url) }
                case .failure(let error):
                    if (error as NSError).code != NSUserCancelledError {
                        instance.runtime.errorMessage = error.localizedDescription
                    }
                }
            }
            .onChange(of: library.creationRequested) { _, requested in
                guard requested, isKey else { return }
                library.creationRequested = false
                if SimulatorInstanceActions.canCreateReason(library) == nil { creating = true }
            }
            .onChange(of: library.requestedIdentityID) { _, id in
                guard isKey, let id else { return }
                library.requestedIdentityID = nil
                editingIdentity = library.instances.first { $0.id == id }
            }
            .onChange(of: library.requestedResetID) { _, id in
                guard isKey, let id else { return }
                library.requestedResetID = nil
                resetting = library.instances.first { $0.id == id }
            }
            .onChange(of: library.requestedDeleteID) { _, id in
                guard isKey, let id else { return }
                library.requestedDeleteID = nil
                deleting = library.instances.first { $0.id == id }
            }
    }

    private func dialogs(_ content: some View) -> some View {
        content
            .sheet(isPresented: $creating) { SimulatorInstanceCreationSheet(library: library) }
            .sheet(item: $editingIdentity) { SimulatorInstanceIdentitySheet(library: library, instance: $0) }
            .confirmationDialog("删除设备实例", isPresented: Binding(get: { deleting != nil },
                set: { if !$0 { deleting = nil } }), titleVisibility: .visible, presenting: deleting) { instance in
                Button("删除「\(instance.record.name)」", role: .destructive) {
                    let id = instance.id
                    deleting = nil
                    Task { await library.delete(id) }
                }
                Button("取消", role: .cancel) { deleting = nil }
            } message: { instance in
                Text("将停止并移除「\(instance.record.name)」，同时删除这台设备的独立数据盘、固件设置和配对记录。已导入的固件包会保留，可重新创建实例。")
            }
            .confirmationDialog("重置实例", isPresented: Binding(get: { resetting != nil },
                set: { if !$0 { resetting = nil } }), titleVisibility: .visible, presenting: resetting) { instance in
                Button("重置「\(instance.record.name)」") {
                    let id = instance.id
                    resetting = nil
                    Task { await library.resetInstance(id) }
                }
                Button("取消", role: .cancel) { resetting = nil }
            } message: { instance in
                Text("将停止「\(instance.record.name)」并清除运行缓存与错误。已保存的数据、固件设置和已导入固件都会保留。")
            }
    }

    private func alerts(_ content: some View) -> some View {
        content
            .alert("设备实例", isPresented: Binding(get: { isKey && library.errorMessage != nil },
                set: { if !$0 { library.errorMessage = nil } })) {
                Button("好") { library.errorMessage = nil }
            } message: { Text(library.errorMessage ?? "") }
            .sheet(item: Binding(
                get: { isKey ? presentedInstance?.runReview : nil },
                set: { presentedInstance?.runReview = $0 }
            )) { prepared in
                SimulatorRunConfirmation(summary: prepared.summary, runLimitSeconds: prepared.runLimitSeconds) {
                    presentedInstance?.runReview = nil
                } confirm: {
                    if let instance = presentedInstance {
                        SimulatorInstanceActions.confirmRun(instance, review: prepared)
                    }
                }
            }
            .alert(presentedInstance?.record.name ?? "设备实例", isPresented: Binding(
                get: { isKey && instanceError != nil },
                set: { if !$0 { dismissInstanceErrors() } }
            )) {
                Button("好") { dismissInstanceErrors() }
            } message: { Text(instanceError ?? "") }
    }

    private var instanceError: String? {
        guard let instance = presentedInstance else { return nil }
        return instance.sharing.errorMessage ?? instance.runtime.errorMessage ?? instance.runtime.runtimeSession.errorMessage
    }

    private func dismissInstanceErrors() {
        guard let instance = presentedInstance else { return }
        instance.sharing.errorMessage = nil
        instance.runtime.errorMessage = nil
        instance.runtime.runtimeSession.errorMessage = nil
    }
}

private struct SimulatorInspectorAttachment: ViewModifier {
    let library: SimulatorInstanceLibrary
    @Binding var presented: Bool

    func body(content: Content) -> some View {
        if let instance = library.selected {
            content.inspector(isPresented: $presented) {
                SimulatorInstanceInspector(library: library, instance: instance)
                    .id(instance.id)
                    .inspectorColumnWidth(min: 320, ideal: 380, max: 520)
            }
        } else {
            content
        }
    }
}

@MainActor struct SimulatorAuxiliaryDeviceWindow: View {
    let library: SimulatorInstanceLibrary
    let instanceID: UUID
    @State private var inspectorPresented = true
    @State private var logsPresented = false
    @State private var importFirmware = false
    @State private var creating = false
    @State private var editingIdentity: SimulatorInstanceContext?
    @State private var deleting: SimulatorInstanceContext?
    @State private var resetting: SimulatorInstanceContext?

    private var instance: SimulatorInstanceContext? { library.instances.first { $0.id == instanceID } }

    var body: some View {
        Group {
            if let instance {
                SimulatorDeviceDetailView(instance: instance, logsPresented: $logsPresented)
                    .inspector(isPresented: $inspectorPresented) {
                        SimulatorInstanceInspector(library: library, instance: instance)
                            .id(instance.id)
                            .inspectorColumnWidth(min: 320, ideal: 380, max: 520)
                    }
                    .navigationTitle(instance.record.name)
                    .focusedSceneValue(\.simulatorLibrary, library)
                    .focusedSceneValue(\.simulatorInstance, instance)
                    .focusedSceneValue(\.simulatorInspectorPresented, $inspectorPresented)
                    .focusedSceneValue(\.simulatorLogsPresented, $logsPresented)
                    .focusedSceneValue(\.simulatorImportFirmware, $importFirmware)
                    .toolbar {
                        SimulatorInstanceActionToolbar(
                            library: library, instance: instance,
                            onCreate: { creating = true }, onImport: { importFirmware = true }
                        )
                    }
            } else {
                ContentUnavailableView("设备实例已删除", systemImage: "applewatch")
            }
        }
        .modifier(SimulatorScenePresentations(
            library: library, sceneInstance: instance, importFirmware: $importFirmware,
            creating: $creating, editingIdentity: $editingIdentity, deleting: $deleting, resetting: $resetting
        ))
        .background {
            ForEach(library.instances) { item in
                SimulatorInstanceRuntimeHost(library: library, instance: item)
            }
        }
    }
}

@MainActor struct SimulatorDeviceDetailView: View {
    let instance: SimulatorInstanceContext
    @Binding var logsPresented: Bool
    @Environment(\.openWindow) private var openWindow
    private var session: SimulatorSessionModel { instance.runtime.runtimeSession }

    var body: some View {
        Group {
            if logsPresented {
                VSplitView {
                    canvas.frame(minHeight: 240)
                    SimulatorLogPanel(runtime: instance.runtime, stream: instance.logStream, instanceName: instance.record.name)
                        .frame(minHeight: 120)
                }
            } else {
                canvas
            }
        }
    }

    @ViewBuilder private var canvas: some View {
        if let geometry = session.manifest?.displayGeometry {
            SimulatorNativeCanvas(image: session.image, geometry: geometry, acceptsInput: session.canInput,
                                  onInput: session.sendInput)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .accessibilityIdentifier("simulator.canvas.\(instance.id.uuidString)")
                .contextMenu {
                    Button("截图") { SimulatorInstanceActions.screenshot(instance) }
                        .disabled(session.image == nil)
                    Button("在新窗口打开") { openWindow(id: "device-display", value: instance.id) }
                }
        } else {
            ContentUnavailableView {
                Label(session.isAttached ? "等待画面" : "设备未运行", systemImage: "applewatch")
            } description: {
                Text(session.isAttached ? "设备已连接，正在等待第一帧。" : "在检查器中选择固件并运行，或使用工具栏「运行」。")
            }
        }
    }
}

@MainActor private struct SimulatorInstanceIdentitySheet: View {
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext
    @Environment(\.dismiss) private var dismiss
    @State private var serial = ""
    @State private var address = ""
    @State private var loading = true
    @State private var failureMessage: String?

    var body: some View {
        NavigationStack {
            Form {
                LabeledContent("实例", value: instance.record.name)
                TextField("序列号", text: $serial)
                    .accessibilityIdentifier("simulator.virtual-serial")
                Text("供设备信息和应用识别，最多 \(SimulatorVirtualIdentity.serialLimit(model: instance.record.model)) 位英文字符。")
                TextField("蓝牙地址", text: $address, prompt: Text("02:12:34:56:78:9A"))
                    .accessibilityIdentifier("simulator.virtual-address")
                Text("供固件识别此虚拟设备；不改变 Mac 对外广播的物理地址或系统配对身份。")
                Text("下次启动使用新身份和全新设备数据，需要重新配对。原应用、表盘、设置及配对记录保留在旧快照中。身份未改变时不会重建数据。")
                if loading { ProgressView() }
                if let failureMessage { Text(failureMessage).foregroundStyle(.red) }
            }
            .formStyle(.grouped)
            .navigationTitle("虚拟设备身份")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }
                        .keyboardShortcut(.defaultAction)
                }
                ToolbarItem(placement: .destructiveAction) {
                    Button("应用新身份并重建数据", role: .destructive) {
                        do {
                            try library.setVirtualIdentity(instance.id, identity: .init(serial: serial, address: address))
                            dismiss()
                        } catch { failureMessage = error.localizedDescription }
                    }
                    .disabled(loading || serial.isEmpty || address.isEmpty || instance.runtime.isBusy)
                    .accessibilityIdentifier("simulator.save-virtual-identity")
                }
            }
        }
        .frame(minWidth: 480, minHeight: 360)
        .task {
            defer { loading = false }
            do {
                let identity: SimulatorVirtualIdentity
                if let requested = instance.record.virtualIdentity { identity = requested }
                else { identity = try await instance.runtime.savedVirtualIdentity() ?? .generated(for: instance.id) }
                serial = identity.serial; address = identity.address
            } catch { failureMessage = error.localizedDescription }
        }
    }
}

@MainActor private struct SimulatorInstanceCreationSheet: View {
    let library: SimulatorInstanceLibrary
    @Environment(\.dismiss) private var dismiss
    @State private var newModel: String
    @State private var nameDraft: SimulatorInstanceNameDraft
    @State private var serial: String
    @State private var address: String
    @State private var skipOOBE = true
    @State private var failureMessage: String?

    init(library: SimulatorInstanceLibrary) {
        self.library = library
        let model = library.deviceModels.first?.model ?? ""
        _newModel = State(initialValue: model)
        _nameDraft = State(initialValue: SimulatorInstanceNameDraft(defaultName: library.defaultName(for: model)))
        let identity = SimulatorVirtualIdentity.generated(for: UUID())
        _serial = State(initialValue: identity.serial)
        _address = State(initialValue: identity.address)
    }

    private var selectedModel: BundledDevicePreset? { library.deviceModels.first { $0.model == newModel } }

    var body: some View {
        NavigationStack {
            Form {
                Picker("设备型号", selection: $newModel) {
                    ForEach(library.deviceModels, id: \.model) { item in
                        Text(SimulatorInstanceLibrary.deviceName(item)).tag(item.model)
                    }
                }.accessibilityIdentifier("simulator.new-instance-model")
                TextField("实例名称", text: Binding(get: { nameDraft.text }, set: { nameDraft.edit($0); failureMessage = nil }),
                          prompt: Text(nameDraft.defaultName))
                    .accessibilityIdentifier("simulator.new-instance-name")
                    .help("用于实例列表、设备标题和录屏文件名；不会修改固件型号或序列号")
                TextField("虚拟序列号", text: $serial)
                    .accessibilityIdentifier("simulator.new-instance-serial")
                TextField("虚拟蓝牙地址", text: $address)
                    .accessibilityIdentifier("simulator.new-instance-address")
                Text("序列号供固件和应用识别；虚拟地址属于模拟设备，不改变 Mac 的物理蓝牙身份。")
                Toggle("跳过首次设置（OOBE）", isOn: $skipOOBE)
                    .accessibilityIdentifier("simulator.new-instance-skip-oobe")
                Text(skipOOBE ? "首次启动直接进入设备界面。" : "保留语言选择和配对引导，可开启外部蓝牙后自行连接。")
                if let failureMessage {
                    Text(failureMessage).foregroundStyle(.red)
                        .accessibilityIdentifier("simulator.instance-name-error")
                }
            }
            .formStyle(.grouped)
            .navigationTitle("新建设备实例")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("创建") {
                        guard let selectedModel else { return }
                        do {
                            try library.createNamed(model: selectedModel.model, name: nameDraft.name,
                                                    identity: .init(serial: serial, address: address), skipOOBE: skipOOBE)
                            dismiss()
                        } catch { failureMessage = error.localizedDescription }
                    }
                    .disabled(selectedModel == nil || nameDraft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .accessibilityIdentifier("simulator.confirm-create-instance")
                    .keyboardShortcut(.defaultAction)
                }
            }
            .onChange(of: library.deviceModels.map(\.model), initial: true) { _, models in
                if !models.contains(newModel) { newModel = models.first ?? "" }
            }
            .onChange(of: newModel) { _, model in
                nameDraft.updateDefault(library.defaultName(for: model))
                failureMessage = nil
            }
        }
        .frame(minWidth: 440, minHeight: 420)
    }
}

@MainActor private struct SimulatorInstanceRenameSheet: View {
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext
    @Environment(\.dismiss) private var dismiss
    @State private var name: String
    @State private var failureMessage: String?

    init(library: SimulatorInstanceLibrary, instance: SimulatorInstanceContext) {
        self.library = library
        self.instance = instance
        _name = State(initialValue: instance.record.name)
    }

    var body: some View {
        NavigationStack {
            Form {
                TextField("实例名称", text: $name)
                    .accessibilityIdentifier("simulator.rename-instance-name")
                if let failureMessage {
                    Text(failureMessage).foregroundStyle(.red)
                        .accessibilityIdentifier("simulator.instance-name-error")
                }
            }
            .formStyle(.grouped)
            .navigationTitle("重命名实例")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("保存") {
                        do { try library.rename(instance.id, name: name); dismiss() }
                        catch { failureMessage = error.localizedDescription }
                    }
                    .disabled(name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("simulator.confirm-rename-instance")
                }
            }
            .onChange(of: name) { _, _ in failureMessage = nil }
        }
        .frame(minWidth: 400, minHeight: 180)
    }
}
