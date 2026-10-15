import AppKit
import SwiftUI

@MainActor struct SimulatorInstanceManagerView: View {
    @Bindable var library: SimulatorInstanceLibrary
    @State private var creating = false
    @State private var renaming: SimulatorInstanceContext?
    @State private var editingIdentity: SimulatorInstanceContext?
    @State private var displaySelection = SimulatorDisplaySelection()
    @State private var workspaceID = UUID()

    private var availableIDs: Set<UUID> { Set(library.instances.map(\.id)) }
    private var displayableIDs: Set<UUID> {
        Set(library.instances.filter { $0.runtime.runtimeSession.isRunning || $0.runtime.runtimeSession.image != nil }.map(\.id))
    }

    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("设备实例").font(.headline)
                    Spacer()
                    Button { presentCreation() } label: { Image(systemName: "plus") }
                        .help("创建新实例")
                        .accessibilityIdentifier("simulator.create-instance")
                        .disabled(!library.storageAvailable || library.deviceModels.isEmpty)
                }.padding(.horizontal, 12).padding(.top, 16)
                Text("\(library.instances.count) 台设备 · \(library.runningCount) 台运行中")
                    .font(.caption).foregroundStyle(.secondary).padding(.horizontal, 12)
                List(selection: $library.selectedID) {
                    ForEach(library.instances) { instance in
                        VStack(alignment: .leading, spacing: 5) {
                            HStack {
                                Image(systemName: instance.runtime.isServing ? "play.circle.fill" : "applewatch")
                                    .foregroundStyle(instance.runtime.isServing ? .green : .secondary)
                                Text(instance.record.name).lineLimit(2)
                            }
                            Text(instance.record.model).font(.caption2).foregroundStyle(.secondary)
                            Text(instance.runtime.runtimeSession.stateLabel).font(.caption).foregroundStyle(.secondary)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.vertical, 5).tag(instance.id)
                            .contentShape(Rectangle())
                            .overlay {
                                SimulatorInstanceDragSource(
                                    payload: .init(workspaceID: workspaceID, instanceID: instance.id),
                                    canDrag: displayableIDs.contains(instance.id),
                                    canShow: displayableIDs.contains(instance.id) && !displaySelection.visibleIDs.contains(instance.id),
                                    select: {
                                        library.selectedID = instance.id
                                        displaySelection.select(instance.id, availableIDs: availableIDs, displayableIDs: displayableIDs)
                                    }, show: { pinDisplay(instance.id, origin: "context-menu") },
                                    rename: { renaming = instance })
                                    .accessibilityHidden(true)
                            }
                            .contextMenu {
                                Button("重命名…") { renaming = instance }
                                    .accessibilityIdentifier("simulator.rename-instance.\(instance.id.uuidString)")
                                if !displaySelection.visibleIDs.contains(instance.id), displayableIDs.contains(instance.id) {
                                    Button("显示画面") { pinDisplay(instance.id, origin: "context-menu") }
                                }
                            }
                            .accessibilityIdentifier("simulator.instance.\(instance.id.uuidString)")
                    }
                }
                Button("虚拟设备身份…") { editingIdentity = library.selected }
                    .disabled(library.selected == nil || !library.storageAvailable)
                    .accessibilityIdentifier("simulator.edit-virtual-identity")
                    .padding(.horizontal, 12).padding(.bottom, 12)
            }.frame(minWidth: 180, idealWidth: 200, maxWidth: 240)
            ZStack {
                if library.instances.isEmpty {
                    ContentUnavailableView {
                        Label("创建你的第一台设备", systemImage: "applewatch")
                    } description: {
                        if library.catalog.hasLoadedCatalog && library.deviceModels.isEmpty {
                            Text(library.catalog.libraryStatus)
                        }
                    } actions: {
                        Button("新建设备…") { presentCreation() }
                            .disabled(!library.storageAvailable || library.deviceModels.isEmpty)
                        if library.catalog.hasLoadedCatalog && library.deviceModels.isEmpty {
                            Button("重新读取固件目录") { Task { await library.catalog.loadCatalog() } }
                        }
                    }
                } else if library.selected == nil {
                    ContentUnavailableView("选择设备实例", systemImage: "sidebar.left")
                }
                ForEach(library.instances) { instance in
                    VStack(spacing: 0) {
                    SimulatorDeviceConnectionView(library: library, instance: instance, bluetooth: instance.bluetooth)
                    SimulatorWorkspaceView(runtime: instance.runtime, sharing: instance.sharing,
                        presentation: .controlsAndLogs, isActive: library.selectedID == instance.id,
                        instanceName: instance.record.name, recording: instance.recording)
                    }
                        .opacity(library.selectedID == instance.id ? 1 : 0)
                        .allowsHitTesting(library.selectedID == instance.id)
                        .disabled(library.selectedID != instance.id)
                        .accessibilityHidden(library.selectedID != instance.id)
                        .onChange(of: instance.runtime.selectedProfileID) { _, _ in library.persistSelection(instance) }
                        .onChange(of: instance.runtime.runtimeSession.isRunning, initial: true) { _, running in
                            if running { pinDisplay(instance.id, origin: "running") }
                            else { instance.bluetooth.stop() }
                        }
                        .onChange(of: instance.runtime.isStopping) { _, stopping in
                            if stopping { instance.bluetooth.stop() }
                        }
                }
            }.frame(minWidth: 420, idealWidth: 460, maxWidth: 560, minHeight: 620)
            SimulatorInstanceDisplayArea(library: library, selection: $displaySelection, workspaceID: workspaceID)
                .frame(minWidth: 240, idealWidth: 580, maxWidth: .infinity, minHeight: 620)
        }
        .task { await library.catalog.loadCatalog() }
        .onChange(of: library.selectedID, initial: true) { _, id in
            displaySelection.select(id, availableIDs: availableIDs, displayableIDs: displayableIDs)
        }
        .onChange(of: library.instances.map(\.id)) { _, _ in
            displaySelection.reconcile(availableIDs: availableIDs)
        }
        .sheet(isPresented: $creating) {
            SimulatorInstanceCreationSheet(library: library)
        }
        .sheet(item: $renaming) { instance in
            SimulatorInstanceRenameSheet(library: library, instance: instance)
        }
        .sheet(item: $editingIdentity) { instance in
            SimulatorInstanceIdentitySheet(library: library, instance: instance)
        }
        .alert("设备实例", isPresented: Binding(get: { library.errorMessage != nil },
            set: { if !$0 { library.errorMessage = nil } })) {
            Button("确定") { library.errorMessage = nil }
        } message: { Text(library.errorMessage ?? "") }
    }

    private func pinDisplay(_ id: UUID, origin: String) {
        guard displayableIDs.contains(id) else { return }
        let added = displaySelection.pin(id, availableIDs: availableIDs)
        NSLog("[AzkiDisplayDrag] pin origin=%@ instance=%@ added=%d", origin, id.uuidString, added ? 1 : 0)
    }

    private func presentCreation() { creating = true }
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
        VStack(alignment: .leading, spacing: 16) {
            Text("虚拟设备身份").font(.title2)
            Text(instance.record.name).foregroundStyle(.secondary)
            TextField("序列号", text: $serial).textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("simulator.virtual-serial")
            Text("供设备信息和应用识别，最多 \(SimulatorVirtualIdentity.serialLimit(model: instance.record.model)) 位英文字符。")
                .font(.caption).foregroundStyle(.secondary)
            TextField("蓝牙地址，例如 02:12:34:56:78:9A", text: $address).textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("simulator.virtual-address")
            Text("供固件识别此虚拟设备；不改变 Mac 对外广播的物理地址或系统配对身份。")
                .font(.caption).foregroundStyle(.secondary)
            Text("下次启动使用新身份和全新设备数据，需要重新配对。原应用、表盘、设置及配对记录保留在旧快照中。身份未改变时不会重建数据。")
                .font(.callout).fixedSize(horizontal: false, vertical: true)
            if loading { ProgressView().controlSize(.small) }
            if let failureMessage { Text(failureMessage).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true) }
            HStack {
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("应用新身份并重建数据") {
                    do {
                        try library.setVirtualIdentity(instance.id, identity: .init(serial: serial, address: address))
                        dismiss()
                    } catch { failureMessage = error.localizedDescription }
                }.disabled(loading || serial.isEmpty || address.isEmpty || instance.runtime.isBusy)
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("simulator.save-virtual-identity")
            }
        }.padding(24).frame(width: 480)
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

/// Own the picker state in the sheet so its first render and Create validation
/// use the same model, rather than a parent state value captured before opening.
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

    private var selectedModel: BundledFirmwareItem? { library.deviceModels.first { $0.model == newModel } }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
                Text("新建设备实例").font(.title2)
                Picker("设备型号", selection: $newModel) {
                    ForEach(library.deviceModels, id: \.model) { item in
                        Text(SimulatorInstanceLibrary.deviceName(item)).tag(item.model)
                    }
                }.accessibilityIdentifier("simulator.new-instance-model")
                TextField("实例名称", text: Binding(get: { nameDraft.text }, set: { nameDraft.edit($0); failureMessage = nil }),
                          prompt: Text(nameDraft.defaultName))
                    .textFieldStyle(.roundedBorder)
                    .accessibilityIdentifier("simulator.new-instance-name")
                    .help("用于实例列表、设备标题和录屏文件名；不会修改固件型号或序列号")
                TextField("虚拟序列号", text: $serial).textFieldStyle(.roundedBorder)
                    .accessibilityIdentifier("simulator.new-instance-serial")
                TextField("虚拟蓝牙地址", text: $address).textFieldStyle(.roundedBorder)
                    .accessibilityIdentifier("simulator.new-instance-address")
                Text("序列号供固件和应用识别；虚拟地址属于模拟设备，不改变 Mac 的物理蓝牙身份。")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("跳过首次设置（OOBE）", isOn: $skipOOBE)
                    .accessibilityIdentifier("simulator.new-instance-skip-oobe")
                Text(skipOOBE ? "首次启动直接进入设备界面。" : "保留语言选择和配对引导，可开启外部蓝牙后自行连接。")
                    .font(.caption).foregroundStyle(.secondary)
                if let failureMessage {
                    Text(failureMessage).font(.callout).foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("simulator.instance-name-error")
                }
                HStack {
                    Spacer()
                    Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                    Button("创建") {
                        guard let selectedModel else { return }
                        do {
                            try library.createNamed(model: selectedModel.model, name: nameDraft.name,
                                                    identity: .init(serial: serial, address: address), skipOOBE: skipOOBE)
                            dismiss()
                        } catch { failureMessage = error.localizedDescription }
                    }.disabled(selectedModel == nil || nameDraft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        .accessibilityIdentifier("simulator.confirm-create-instance")
                        .keyboardShortcut(.defaultAction)
                }
            }.padding(24).frame(width: 440)
            .onChange(of: library.deviceModels.map(\.model), initial: true) { _, models in
                if !models.contains(newModel) { newModel = models.first ?? "" }
            }
            .onChange(of: newModel) { _, model in
                nameDraft.updateDefault(library.defaultName(for: model))
                failureMessage = nil
            }
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
        VStack(alignment: .leading, spacing: 18) {
            Text("重命名实例").font(.title2)
            TextField("实例名称", text: $name)
                .textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("simulator.rename-instance-name")
            if let failureMessage {
                Text(failureMessage).font(.callout).foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("simulator.instance-name-error")
            }
            HStack {
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("保存") {
                    do { try library.rename(instance.id, name: name); dismiss() }
                    catch { failureMessage = error.localizedDescription }
                }
                .disabled(name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .keyboardShortcut(.defaultAction)
                .accessibilityIdentifier("simulator.confirm-rename-instance")
            }
        }
        .padding(24).frame(width: 440)
        .onChange(of: name) { _, _ in failureMessage = nil }
    }
}

/// Own the complete local mouse sequence. A drag changes display placement
/// only on release over this window's display area; a short click selects.
@MainActor private struct SimulatorInstanceDragSource: NSViewRepresentable {
    let payload: SimulatorInstanceDragPayload
    let canDrag: Bool
    let canShow: Bool
    let select: () -> Void
    let show: () -> Void
    let rename: () -> Void

    func makeNSView(context: Context) -> SimulatorInstanceDragSourceView {
        let view = SimulatorInstanceDragSourceView()
        view.setAccessibilityElement(false)
        return view
    }

    func updateNSView(_ view: SimulatorInstanceDragSourceView, context: Context) {
        view.payload = payload
        view.canDrag = canDrag; view.canShow = canShow
        view.selectInstance = select; view.showInstance = show; view.renameInstance = rename
        view.toolTip = canDrag ? "拖到右侧显示画面" : nil
    }
}

@MainActor final class SimulatorInstanceDragSourceView: NSView {
    var payload: SimulatorInstanceDragPayload?
    var canDrag = false
    var canShow = false
    var selectInstance: (() -> Void)?
    var showInstance: (() -> Void)?
    var renameInstance: (() -> Void)?
    private var gesture: SimulatorInstanceDragGesture?
    private weak var gestureWindow: NSWindow?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
        cancelInternalDrag()
        guard let window, let payload, event.windowNumber == window.windowNumber else { return }
        gesture = SimulatorInstanceDragGesture(payload: payload, sourceWindowNumber: window.windowNumber,
                                               start: event.locationInWindow)
        gestureWindow = window
        NotificationCenter.default.addObserver(self, selector: #selector(interrupted(_:)),
            name: NSWindow.didResignKeyNotification, object: window)
        NotificationCenter.default.addObserver(self, selector: #selector(interrupted(_:)),
            name: NSApplication.didResignActiveNotification, object: nil)
        NSLog("[AzkiDisplayDrag] internal.down instance=%@ enabled=%d eventWindow=%@ window=%ld", payload.instanceID.uuidString,
              canDrag ? 1 : 0, NSStringFromPoint(event.locationInWindow), window.windowNumber)
    }

    override func mouseDragged(with event: NSEvent) {
        let wasDragging = gesture?.isDragging == true
        guard let gesture = updateGesture(event), let window = gestureWindow, gesture.isDragging, canDrag else { return }
        if !wasDragging {
            setSourceHighlighted(true)
            NSLog("[AzkiDisplayDrag] internal.begin instance=%@ eventWindow=%@", gesture.payload.instanceID.uuidString,
                  NSStringFromPoint(event.locationInWindow))
        }
        SimulatorInstanceInternalDragRegistry.updateHighlight(for: gesture, window: window, point: event.locationInWindow)
    }

    override func mouseUp(with event: NSEvent) {
        guard let gesture = updateGesture(event), let window = gestureWindow else { return }
        defer { cancelInternalDrag() }
        if gesture.isDragging {
            let accepted: Bool
            if canDrag, let target = SimulatorInstanceInternalDragRegistry.target(for: gesture, window: window,
                                                                                  point: event.locationInWindow) {
                accepted = target.receive(gesture, pointInWindow: event.locationInWindow)
            } else { accepted = false }
            NSLog("[AzkiDisplayDrag] internal.end instance=%@ accepted=%d eventWindow=%@", gesture.payload.instanceID.uuidString,
                  accepted ? 1 : 0, NSStringFromPoint(event.locationInWindow))
        } else if bounds.contains(convert(event.locationInWindow, from: nil)) {
            selectInstance?()
        }
    }

    private func updateGesture(_ event: NSEvent) -> SimulatorInstanceDragGesture? {
        guard var gesture, let origin = gestureWindow, window === origin, payload == gesture.payload,
              gesture.update(locationInWindow: event.locationInWindow, windowNumber: event.windowNumber) else {
            cancelInternalDrag()
            return nil
        }
        self.gesture = gesture
        return gesture
    }

    override func viewWillMove(toWindow newWindow: NSWindow?) {
        if newWindow !== window { cancelInternalDrag() }
        super.viewWillMove(toWindow: newWindow)
    }

    @objc private func interrupted(_ notification: Notification) { cancelInternalDrag() }

    private func cancelInternalDrag() {
        if gesture?.isDragging == true {
            NSLog("[AzkiDisplayDrag] internal.clear instance=%@", gesture?.payload.instanceID.uuidString ?? "none")
        }
        gesture?.cancel()
        gesture = nil; gestureWindow = nil
        NotificationCenter.default.removeObserver(self, name: NSWindow.didResignKeyNotification, object: nil)
        NotificationCenter.default.removeObserver(self, name: NSApplication.didResignActiveNotification, object: nil)
        setSourceHighlighted(false)
        SimulatorInstanceInternalDragRegistry.clearHighlights()
    }

    private func setSourceHighlighted(_ highlighted: Bool) {
        wantsLayer = true
        layer?.backgroundColor = highlighted ? NSColor.controlAccentColor.withAlphaComponent(0.12).cgColor : nil
        layer?.cornerRadius = 5
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        cancelInternalDrag()
        let menu = NSMenu()
        menu.autoenablesItems = false
        let rename = NSMenuItem(title: "重命名…", action: #selector(renameCurrentInstance), keyEquivalent: "")
        rename.target = self
        menu.addItem(rename)
        if canShow {
            let show = NSMenuItem(title: "显示画面", action: #selector(showCurrentInstance), keyEquivalent: "")
            show.target = self
            menu.addItem(show)
        }
        return menu
    }
    @objc private func renameCurrentInstance() { renameInstance?() }
    @objc private func showCurrentInstance() { if canShow { showInstance?() } }
}
