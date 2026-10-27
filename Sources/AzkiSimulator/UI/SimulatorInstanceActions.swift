import AppKit
import SwiftUI

@MainActor enum SimulatorInstanceActions {
    static func controlsAvailable(_ instance: SimulatorInstanceContext) -> Bool {
        SimulatorWorkspaceView.coreControlsAvailable(runtime: instance.runtime, sharing: instance.sharing)
    }

    static func canRun(_ instance: SimulatorInstanceContext) -> Bool {
        controlsAvailable(instance) && instance.runtime.canRunSelectedFirmware
    }

    static func canCreateReason(_ library: SimulatorInstanceLibrary) -> String? {
        if !library.storageAvailable { return "实例存储不可用" }
        if library.deviceModels.isEmpty {
            return library.catalog.hasLoadedCatalog ? "固件库没有可用的设备型号" : "正在读取设备型号"
        }
        return nil
    }

    static func canStop(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping && (instance.runtime.isServing || instance.runtime.isBusy)
    }

    static func canDetach(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping && !instance.runtime.isServing && !instance.runtime.isBusy
            && instance.runtime.runtimeSession.isAttached
    }

    static func isWatch(_ instance: SimulatorInstanceContext) -> Bool {
        ["miwear.watch.p65", "miwear.watch.q65"].contains(
            instance.runtime.modelConstraint ?? instance.runtime.selectedFirmware?.model)
    }

    static func watchButtonsAvailable(_ instance: SimulatorInstanceContext) -> Bool {
        guard controlsAvailable(instance), !instance.runtime.isStopping, instance.runtime.runtimeSession.isRunning,
              case .array(let capabilities)? = instance.runtime.runtimeSession.status?["capabilities"] else { return false }
        return capabilities.contains(.string("device-button")) && isWatch(instance)
    }

    static func run(_ instance: SimulatorInstanceContext) {
        let runtime = instance.runtime
        Task {
            guard controlsAvailable(instance), runtime.canRunSelectedFirmware else { return }
            guard await runtime.prepareSelectedFirmware(), controlsAvailable(instance) else { return }
            if !runtime.preparedNeedsReview { await runtime.startApproved() }
            else { instance.runReview = SimulatorRunReview(summary: runtime.selectedSummary, runLimitSeconds: runtime.preparedRunLimitSeconds) }
        }
    }

    static func stop(_ instance: SimulatorInstanceContext) {
        Task {
            await instance.recording.stop()
            if instance.runtime.isServing || instance.runtime.isBusy { await instance.runtime.stop() }
        }
    }

    static func detach(_ instance: SimulatorInstanceContext) {
        Task {
            await instance.recording.stop()
            instance.runtime.runtimeSession.detach()
        }
    }

    static func screenshot(_ instance: SimulatorInstanceContext) {
        guard let image = instance.runtime.runtimeSession.image else { return }
        do {
            let url = try SimulatorScreenshot.save(image, instanceName: instance.record.name)
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } catch {
            instance.runtime.errorMessage = error.localizedDescription
        }
    }

    static func toggleRecording(_ instance: SimulatorInstanceContext) {
        let session = instance.runtime.runtimeSession
        if instance.recording.isRecording {
            Task { await instance.recording.stop() }
        } else if session.isRunning, session.image != nil {
            instance.recording.start(session: session, instanceName: instance.record.name)
        }
    }

    static func importFirmware(_ instance: SimulatorInstanceContext) -> Bool {
        controlsAvailable(instance) && instance.runtime.libraryAvailable
            && !instance.runtime.isServing && !instance.runtime.runtimeSession.isAttached
    }

    static func confirmRun(_ instance: SimulatorInstanceContext, review: SimulatorRunReview) {
        instance.runReview = nil
        let runtime = instance.runtime
        guard controlsAvailable(instance), runtime.canStartPrepared, runtime.selectedSummary == review.summary,
              runtime.preparedRunLimitSeconds == review.runLimitSeconds else {
            runtime.errorMessage = "准备的文件已变化，请重新准备并核对指纹。"
            return
        }
        Task { await runtime.startApproved() }
    }
}

@MainActor struct SimulatorInstanceActionToolbar: ToolbarContent {
    let library: SimulatorInstanceLibrary
    var instance: SimulatorInstanceContext?
    var showsCreate: Bool = true
    var onCreate: () -> Void
    var onImport: () -> Void
    var logsPresented: Binding<Bool>
    @Environment(\.openWindow) private var openWindow

    var body: some ToolbarContent {
        if showsCreate {
            ToolbarItem(placement: .primaryAction) {
                Button("新建设备实例", systemImage: "plus") { onCreate() }
                    .help(SimulatorInstanceActions.canCreateReason(library) ?? "创建新实例（⌘N）")
                    .accessibilityLabel("新建设备实例")
                    .accessibilityIdentifier("simulator.create-instance")
                    .disabled(SimulatorInstanceActions.canCreateReason(library) != nil)
            }
        }
        ToolbarItemGroup(placement: .primaryAction) {
            Button("存储管理", systemImage: "externaldrive") { openWindow(id: "storage") }
                .help("查看和管理实例快照、固件库与临时会话")
                .accessibilityIdentifier("simulator.storage")
            if let instance {
                Button("运行", systemImage: "play.fill") { SimulatorInstanceActions.run(instance) }
                    .help(instance.runtime.runUnavailableReason ?? "按本实例的硬件预设开机（⌘R）")
                    .disabled(!SimulatorInstanceActions.canRun(instance))
                    .accessibilityIdentifier("simulator.run-firmware")
                Button("停止", systemImage: "stop.fill") { SimulatorInstanceActions.stop(instance) }
                    .help(instance.runtime.isStopping ? "正在停止…" : "停止本应用启动的设备（⌘.）")
                    .disabled(!SimulatorInstanceActions.canStop(instance))
                    .accessibilityIdentifier("simulator.stop")
                Button(logsPresented.wrappedValue ? "隐藏日志" : "日志",
                       systemImage: logsPresented.wrappedValue ? "rectangle.bottomthird.inset.filled" : "text.alignleft") {
                    logsPresented.wrappedValue.toggle()
                }
                .help(logsPresented.wrappedValue ? "隐藏实时日志" : "显示实时日志")
                .accessibilityIdentifier("simulator.logs")
                Button("导入固件", systemImage: "square.and.arrow.down") { onImport() }
                    .help("选择完整 OTA 包导入到固件库")
                    .disabled(!SimulatorInstanceActions.importFirmware(instance))
                    .accessibilityIdentifier("simulator.import-firmware")
                Button("截图", systemImage: "camera") { SimulatorInstanceActions.screenshot(instance) }
                    .help("保存当前设备画面截图")
                    .disabled(instance.runtime.runtimeSession.image == nil)
                Button(instance.recording.isRecording ? "停止录屏" : "录屏",
                       systemImage: instance.recording.isRecording ? "stop.circle" : "record.circle") {
                    SimulatorInstanceActions.toggleRecording(instance)
                }
                .help("录制此设备的画面，完成后保存到桌面")
                .disabled(instance.recording.isFinishing
                          || (!instance.recording.isRecording && (instance.runtime.runtimeSession.image == nil
                                                                 || !instance.runtime.runtimeSession.isRunning)))
                .accessibilityIdentifier("simulator.recording")
                if SimulatorInstanceActions.isWatch(instance) {
                    Button("表冠", systemImage: "button.programmable") {
                        instance.runtime.runtimeSession.sendDeviceButton("crown")
                    }
                    .help("按下表冠（旋转表盘/打开应用抽屉）")
                    .disabled(!SimulatorInstanceActions.watchButtonsAvailable(instance))
                    .accessibilityIdentifier("simulator.watch.crown")
                    Button("下键", systemImage: "hand.tap") {
                        instance.runtime.runtimeSession.sendDeviceButton("lower")
                    }
                    .help("按下下键（返回/息屏）")
                    .disabled(!SimulatorInstanceActions.watchButtonsAvailable(instance))
                    .accessibilityIdentifier("simulator.watch.lower.single")
                }
                Button("新窗口", systemImage: "macwindow") { openWindow(id: "device-display", value: instance.id) }
                    .help("在新窗口打开此设备画面")
                    .accessibilityIdentifier("simulator.open-display-window")
            }
        }
    }
}

private struct SimulatorLibraryKey: FocusedValueKey { typealias Value = SimulatorInstanceLibrary }
private struct SimulatorInstanceKey: FocusedValueKey { typealias Value = SimulatorInstanceContext }
private struct SimulatorInspectorPresentedKey: FocusedValueKey { typealias Value = Binding<Bool> }
private struct SimulatorLogsPresentedKey: FocusedValueKey { typealias Value = Binding<Bool> }
private struct SimulatorImportFirmwareKey: FocusedValueKey { typealias Value = Binding<Bool> }

extension FocusedValues {
    var simulatorLibrary: SimulatorInstanceLibrary? {
        get { self[SimulatorLibraryKey.self] }
        set { self[SimulatorLibraryKey.self] = newValue }
    }
    var simulatorInstance: SimulatorInstanceContext? {
        get { self[SimulatorInstanceKey.self] }
        set { self[SimulatorInstanceKey.self] = newValue }
    }
    var simulatorInspectorPresented: Binding<Bool>? {
        get { self[SimulatorInspectorPresentedKey.self] }
        set { self[SimulatorInspectorPresentedKey.self] = newValue }
    }
    var simulatorLogsPresented: Binding<Bool>? {
        get { self[SimulatorLogsPresentedKey.self] }
        set { self[SimulatorLogsPresentedKey.self] = newValue }
    }
    var simulatorImportFirmware: Binding<Bool>? {
        get { self[SimulatorImportFirmwareKey.self] }
        set { self[SimulatorImportFirmwareKey.self] = newValue }
    }
}

@MainActor struct SimulatorCommands: Commands {
    @FocusedValue(\.simulatorLibrary) private var library
    @FocusedValue(\.simulatorInstance) private var instance
    @FocusedBinding(\.simulatorInspectorPresented) private var inspectorPresented
    @FocusedBinding(\.simulatorLogsPresented) private var logsPresented
    @FocusedBinding(\.simulatorImportFirmware) private var importFirmware
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("新建设备实例…") { library?.creationRequested = true }
                .keyboardShortcut("n")
                .disabled(library.map { SimulatorInstanceActions.canCreateReason($0) != nil } ?? true)
            Button("导入固件…") { importFirmware = true }
                .disabled(instance.map { !SimulatorInstanceActions.importFirmware($0) } ?? true)
        }
        CommandMenu("设备") {
            Button("运行") { if let instance { SimulatorInstanceActions.run(instance) } }
                .keyboardShortcut("r")
                .disabled(instance.map { !SimulatorInstanceActions.canRun($0) } ?? true)
            Button("停止") { if let instance { SimulatorInstanceActions.stop(instance) } }
                .keyboardShortcut(".")
                .disabled(instance.map { !SimulatorInstanceActions.canStop($0) } ?? true)
            Button("断开") { if let instance { SimulatorInstanceActions.detach(instance) } }
                .disabled(instance.map { !SimulatorInstanceActions.canDetach($0) } ?? true)
            Divider()
            Button("截图") { if let instance { SimulatorInstanceActions.screenshot(instance) } }
                .disabled(instance?.runtime.runtimeSession.image == nil)
            Button(instance?.recording.isRecording == true ? "停止录屏" : "录屏") {
                if let instance { SimulatorInstanceActions.toggleRecording(instance) }
            }
            .disabled(instance.map {
                $0.recording.isFinishing || (!$0.recording.isRecording && ($0.runtime.runtimeSession.image == nil || !$0.runtime.runtimeSession.isRunning))
            } ?? true)
            Divider()
            Button("点击表冠") { instance?.runtime.runtimeSession.sendDeviceButton("crown") }
                .disabled(instance.map { !SimulatorInstanceActions.watchButtonsAvailable($0) } ?? true)
            Button("单击下键") { instance?.runtime.runtimeSession.sendDeviceButton("lower") }
                .disabled(instance.map { !SimulatorInstanceActions.watchButtonsAvailable($0) } ?? true)
            Button("双击下键") {
                instance?.runtime.runtimeSession.perform("device-button", arguments: ["button": .string("lower"), "clicks": .number(2)])
            }
            .disabled(instance.map { !SimulatorInstanceActions.watchButtonsAvailable($0) } ?? true)
            Divider()
            Button("虚拟设备身份…") { library?.requestedIdentityID = instance?.id }
                .disabled(instance == nil || library?.storageAvailable != true)
            Button("重置实例…") { library?.requestedResetID = instance?.id }
                .disabled(instance == nil)
            Button("删除实例…") { library?.requestedDeleteID = instance?.id }
                .disabled(instance == nil || library?.storageAvailable != true)
        }
        CommandMenu("存储") {
            Button("存储管理…") { openWindow(id: "storage") }
                .keyboardShortcut("s", modifiers: [.command, .shift])
        }
        CommandMenu("显示") {
            Button(inspectorPresented == true ? "隐藏检查器" : "显示检查器") {
                inspectorPresented?.toggle()
            }
            .keyboardShortcut("i", modifiers: [.command, .option])
            .disabled(inspectorPresented == nil || instance == nil)
            Button(logsPresented == true ? "隐藏日志" : "显示日志") {
                logsPresented?.toggle()
            }
            .keyboardShortcut("l", modifiers: [.command, .option])
            .disabled(logsPresented == nil || instance == nil)
            Button("在新窗口打开画面") {
                if let instance { openWindow(id: "device-display", value: instance.id) }
            }
            .disabled(instance == nil)
        }
    }
}
