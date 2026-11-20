import AppKit
import SwiftUI

@MainActor enum SimulatorInstanceActions {
    /// Core controls remain responsive unless the runtime is actively tearing down.
    static func controlsAvailable(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping
    }

    static func canRun(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping
    }

    static func canCreateReason(_ library: SimulatorInstanceLibrary) -> String? {
        if !library.storageAvailable { return "实例存储不可用" }
        return nil
    }

    static func canStop(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping
            && (instance.runtime.isServing
                || instance.runtime.isBusy
                || instance.runtime.runtimeSession.isAttached
                || instance.sharing.isSharing
                || instance.sharing.sessionURL != nil
                || instance.recording.isRecording
                || instance.bluetooth.isActive)
    }

    static func canDetach(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping && instance.runtime.runtimeSession.isAttached
    }

    static func isWatch(_ instance: SimulatorInstanceContext) -> Bool {
        ["miwear.watch.p65", "miwear.watch.q65"].contains(
            instance.runtime.modelConstraint ?? instance.runtime.selectedFirmware?.model)
    }

    static func watchButtonsAvailable(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping && instance.runtime.runtimeSession.isRunning && isWatch(instance)
    }

    static func canScreenshot(_ instance: SimulatorInstanceContext) -> Bool {
        instance.runtime.runtimeSession.image != nil
            || instance.runtime.isServing
            || instance.runtime.runtimeSession.isAttached
    }

    static func canToggleRecording(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.recording.isFinishing
            && (instance.recording.isRecording
                || instance.runtime.isServing
                || instance.runtime.runtimeSession.isAttached)
    }

    /// Smart one-click Run / Restart: automatically resolves conflicting Studio
    /// sharing, loads catalog, picks a ready firmware when none is chosen, or
    /// opens firmware import when the model has no imported firmware yet.
    static func run(
        _ instance: SimulatorInstanceContext,
        library: SimulatorInstanceLibrary? = nil,
        onNeedImport: (() -> Void)? = nil
    ) {
        let runtime = instance.runtime
        Task {
            guard !runtime.isStopping else { return }

            if instance.sharing.isSharing || instance.sharing.sessionURL != nil {
                library?.showToast("已自动关闭 Studio 只读共享，正在准备启动…")
                _ = await instance.sharing.disable()
            }

            if runtime.isServing {
                library?.showToast("正在重新启动「\(instance.record.name)」…")
                await instance.recording.stop()
                await runtime.coldRestart()
                return
            }

            if runtime.runtimeSession.isAttached {
                await instance.recording.stop()
                runtime.runtimeSession.detach()
            }

            if !runtime.hasLoadedCatalog {
                await runtime.loadCatalog()
            }

            let hadSelection = runtime.selectedProfileID != nil
            let hasReady = runtime.autoSelectReadyFirmwareIfNeeded()
            if hasReady && !hadSelection, let name = runtime.selectedFirmware?.displayName {
                library?.showToast("已自动选择固件：\(name)")
            }

            if !runtime.canRunSelectedFirmware {
                if runtime.selectableFirmwareItems.isEmpty, let onNeedImport {
                    library?.showToast("此机型尚未导入固件，请选择固件包导入")
                    onNeedImport()
                } else {
                    let reason = runtime.runUnavailableReason ?? "请先导入或选择可用固件"
                    library?.showToast(reason, isError: true)
                }
                return
            }

            guard await runtime.prepareSelectedFirmware(), !runtime.isStopping else { return }
            if !runtime.preparedNeedsReview {
                await runtime.startApproved()
            } else {
                instance.runReview = SimulatorRunReview(
                    summary: runtime.selectedSummary,
                    runLimitSeconds: runtime.preparedRunLimitSeconds
                )
            }
        }
    }

    static func stop(_ instance: SimulatorInstanceContext, library: SimulatorInstanceLibrary? = nil) {
        Task {
            instance.bluetooth.stop()
            await instance.recording.stop()
            if instance.sharing.isSharing || instance.sharing.sessionURL != nil {
                _ = await instance.sharing.disable()
            }
            if instance.runtime.isServing || instance.runtime.isBusy {
                await instance.runtime.stop()
            } else if instance.runtime.runtimeSession.isAttached {
                instance.runtime.runtimeSession.detach()
                library?.showToast("已断开外部设备会话")
            }
        }
    }

    static func detach(_ instance: SimulatorInstanceContext) {
        Task {
            await instance.recording.stop()
            instance.runtime.runtimeSession.detach()
        }
    }

    static func screenshot(_ instance: SimulatorInstanceContext, library: SimulatorInstanceLibrary? = nil) {
        guard let image = instance.runtime.runtimeSession.image else {
            library?.showToast("设备画面尚未就绪，等待第一帧后再截图")
            return
        }
        let clipGeometry: SimulatorDisplayGeometry? = DisplaySettings.shared.clipToScreenShape
            ? (instance.runtime.runtimeSession.manifest?.displayGeometry
                ?? SimulatorDisplayGeometry(width: image.width, height: image.height))
            : nil
        do {
            let url = try SimulatorScreenshot.save(image, instanceName: instance.record.name, clipGeometry: clipGeometry)
            NSWorkspace.shared.activateFileViewerSelecting([url])
            library?.showToast("截图已保存：\(url.lastPathComponent)")
        } catch {
            instance.runtime.errorMessage = error.localizedDescription
        }
    }

    static func toggleRecording(_ instance: SimulatorInstanceContext, library: SimulatorInstanceLibrary? = nil) {
        let session = instance.runtime.runtimeSession
        if instance.recording.isRecording {
            Task {
                await instance.recording.stop()
                library?.showToast("录屏已停止，正在保存…")
            }
        } else if session.isRunning, let image = session.image {
            let clipGeometry: SimulatorDisplayGeometry? = DisplaySettings.shared.clipToScreenShape
                ? (session.manifest?.displayGeometry
                    ?? SimulatorDisplayGeometry(width: image.width, height: image.height))
                : nil
            instance.recording.start(session: session, instanceName: instance.record.name, clipGeometry: clipGeometry)
            library?.showToast("开始录制设备画面")
        } else {
            library?.showToast("设备尚未输出画面，请等待画面出现后再录屏")
        }
    }

    /// Importing firmware writes to the shared library under a lock and never
    /// interferes with running VMs, so it stays freely accessible.
    static func importFirmware(_ instance: SimulatorInstanceContext) -> Bool {
        !instance.runtime.isStopping
    }

    static func confirmRun(_ instance: SimulatorInstanceContext, review: SimulatorRunReview) {
        instance.runReview = nil
        let runtime = instance.runtime
        guard !runtime.isStopping, runtime.canStartPrepared, runtime.selectedSummary == review.summary,
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
            Button("导入固件", systemImage: "square.and.arrow.down") { onImport() }
                .help("选择固件包导入到固件库（运行期间也可直接导入）")
                .disabled(instance.map { !SimulatorInstanceActions.importFirmware($0) } ?? false)
                .accessibilityIdentifier("simulator.import-firmware")
            if let instance {
                let serving = instance.runtime.isServing
                Button(serving ? "重启" : "运行", systemImage: serving ? "arrow.clockwise" : "play.fill") {
                    SimulatorInstanceActions.run(instance, library: library, onNeedImport: onImport)
                }
                .help(serving ? "重新冷启动当前设备（⌘R）" : (instance.runtime.runUnavailableReason ?? "按本实例的硬件预设开机（⌘R）"))
                .disabled(!SimulatorInstanceActions.canRun(instance))
                .accessibilityIdentifier("simulator.run-firmware")

                Button("停止", systemImage: "stop.fill") {
                    SimulatorInstanceActions.stop(instance, library: library)
                }
                .help(instance.runtime.isStopping ? "正在停止…" : "停止当前设备或断开外部会话（⌘.）")
                .disabled(!SimulatorInstanceActions.canStop(instance))
                .accessibilityIdentifier("simulator.stop")

                Button(logsPresented.wrappedValue ? "隐藏日志" : "日志",
                       systemImage: logsPresented.wrappedValue ? "rectangle.bottomthird.inset.filled" : "text.alignleft") {
                    logsPresented.wrappedValue.toggle()
                }
                .help(logsPresented.wrappedValue ? "隐藏实时日志" : "显示实时日志")
                .accessibilityIdentifier("simulator.logs")

                Button("截图", systemImage: "camera") {
                    SimulatorInstanceActions.screenshot(instance, library: library)
                }
                .help("保存当前设备画面截图")
                .disabled(!SimulatorInstanceActions.canScreenshot(instance))

                Button(instance.recording.isRecording ? "停止录屏" : "录屏",
                       systemImage: instance.recording.isRecording ? "stop.circle.fill" : "record.circle") {
                    SimulatorInstanceActions.toggleRecording(instance, library: library)
                }
                .help("录制此设备的画面，完成后保存到桌面")
                .disabled(!SimulatorInstanceActions.canToggleRecording(instance))
                .accessibilityIdentifier("simulator.recording")

                if SimulatorInstanceActions.isWatch(instance) {
                    Button("表冠", systemImage: "button.programmable") {
                        instance.runtime.runtimeSession.sendDeviceButton("crown")
                    }
                    .help("短按表冠")
                    .disabled(!SimulatorInstanceActions.watchButtonsAvailable(instance))
                    .accessibilityIdentifier("simulator.watch.crown")
                    Button("下键", systemImage: "hand.tap") {
                        instance.runtime.runtimeSession.sendDeviceButton("lower")
                    }
                    .help("短按下键（功能由手表设置决定）")
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
                .disabled(instance.map { !SimulatorInstanceActions.importFirmware($0) } ?? false)
        }
        CommandMenu("设备") {
            Button(instance?.runtime.isServing == true ? "重启" : "运行") {
                if let instance {
                    SimulatorInstanceActions.run(instance, library: library) { importFirmware = true }
                }
            }
            .keyboardShortcut("r")
            .disabled(instance.map { !SimulatorInstanceActions.canRun($0) } ?? true)
            Button("停止") { if let instance { SimulatorInstanceActions.stop(instance, library: library) } }
                .keyboardShortcut(".")
                .disabled(instance.map { !SimulatorInstanceActions.canStop($0) } ?? true)
            Button("断开") { if let instance { SimulatorInstanceActions.detach(instance) } }
                .disabled(instance.map { !SimulatorInstanceActions.canDetach($0) } ?? true)
            Divider()
            Button("截图") { if let instance { SimulatorInstanceActions.screenshot(instance, library: library) } }
                .disabled(instance.map { !SimulatorInstanceActions.canScreenshot($0) } ?? true)
            Button(instance?.recording.isRecording == true ? "停止录屏" : "录屏") {
                if let instance { SimulatorInstanceActions.toggleRecording(instance, library: library) }
            }
            .disabled(instance.map { !SimulatorInstanceActions.canToggleRecording($0) } ?? true)
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
            Toggle("按真实屏幕形状裁切", isOn: Binding(
                get: { DisplaySettings.shared.clipToScreenShape },
                set: { DisplaySettings.shared.clipToScreenShape = $0 }
            ))
            Divider()
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
