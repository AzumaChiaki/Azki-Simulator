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
                .navigationSplitViewColumnWidth(min: 200, ideal: 240, max: 300)
        } detail: {
            detail
        }
        .navigationSplitViewStyle(.balanced)
        .modifier(SimulatorInspectorAttachment(
            library: library,
            presented: $inspectorPresented,
            logsPresented: $logsPresented,
            importFirmware: $importFirmware
        ))
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
                onCreate: { creating = true }, onImport: { importFirmware = true },
                logsPresented: $logsPresented
            )
        }
    }

    @Environment(\.colorScheme) private var colorScheme

    private var sidebar: some View {
        List {
            ForEach(library.instances) { instance in
                GeminiSidebarInstanceRow(
                    instance: instance,
                    deviceTitle: deviceTitle(instance),
                    isSelected: library.selectedID == instance.id,
                    hasError: Self.hasError(instance),
                    onSelect: { library.selectedID = instance.id }
                )
                .listRowInsets(EdgeInsets(top: 3, leading: 6, bottom: 3, trailing: 6))
                .listRowSeparator(.hidden)
                .listRowBackground(Color.clear)
                .contextMenu { instanceMenu(instance) }
                .accessibilityIdentifier("simulator.instance.\(instance.id.uuidString)")
            }
        }
        .listStyle(.sidebar)
        .scrollContentBackground(.hidden)
        .background(GeminiPalette.sidebarBackground(for: colorScheme))
        .navigationTitle("设备实例")
        .navigationSubtitle("\(library.instances.count) 台设备 · \(library.runningCount) 台运行中")
    }

    @ViewBuilder private func instanceMenu(_ instance: SimulatorInstanceContext) -> some View {
        Button(instance.runtime.isServing ? "重新启动" : "运行") {
            SimulatorInstanceActions.run(instance, library: library) { importFirmware = true }
        }
        if SimulatorInstanceActions.canStop(instance) {
            Button("停止") { SimulatorInstanceActions.stop(instance, library: library) }
        }
        Divider()
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
            SimulatorDeviceDetailView(
                library: library,
                instance: instance,
                logsPresented: $logsPresented,
                onImportFirmware: { importFirmware = true }
            )
            .navigationTitle(instance.record.name)
            .navigationSubtitle(instance.runtime.runtimeSession.stateLabel)
        } else if let failure = library.storageFailure {
            ZStack {
                GeminiAtmosphereBackdrop(intensity: .hero)
                GeminiGlassCard(cornerRadius: 20, hoverGlow: false, padding: 28) {
                    VStack(spacing: 14) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.system(size: 34, weight: .light))
                            .foregroundStyle(GeminiPalette.amber)
                        Text("无法读取设备实例").font(.title3.weight(.semibold))
                        Text(failure + "\n原有记录不会被改写；修复后点击重试。")
                            .font(.callout).foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                            .frame(maxWidth: 380)
                        HStack(spacing: 10) {
                            Button("重试") { library.reloadInstances() }
                                .buttonStyle(GeminiNeonButtonStyle(tone: .primary))
                            Button("在访达中显示") {
                                NSWorkspace.shared.activateFileViewerSelecting([SimulatorInstanceRecordStore.defaultRootURL])
                            }
                            .buttonStyle(GeminiNeonButtonStyle(tone: .secondary))
                        }
                        .padding(.top, 4)
                    }
                }
                .padding(32)
            }
        } else if library.instances.isEmpty {
            GeminiEmptyWelcomeStage(
                library: library,
                onCreate: { creating = true },
                onImport: { importFirmware = true }
            )
        } else {
            ZStack {
                GeminiAtmosphereBackdrop(intensity: .stage)
                GeminiGlassCard(cornerRadius: 18, hoverGlow: false, padding: 24) {
                    VStack(spacing: 10) {
                        Image(systemName: "sidebar.left")
                            .font(.system(size: 28, weight: .light))
                            .foregroundStyle(GeminiPalette.accent(for: colorScheme))
                        Text("选择设备实例").font(.headline)
                        Text("从左侧边栏选择一台虚拟设备，或点击工具栏「+」新建。")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
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

// MARK: - Gemini Sidebar Instance Card (Clean Custom Selection, No Native Blue Conflict)

@MainActor private struct GeminiSidebarInstanceRow: View {
    let instance: SimulatorInstanceContext
    let deviceTitle: String
    let isSelected: Bool
    let hasError: Bool
    let onSelect: () -> Void

    @Environment(\.colorScheme) private var colorScheme
    @State private var isHovered = false

    private var isServing: Bool { instance.runtime.isServing }
    private var isRunning: Bool { instance.runtime.runtimeSession.isRunning }

    var body: some View {
        let accent = GeminiPalette.accent(for: colorScheme)
        HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 2, style: .continuous)
                .fill(isSelected ? accent : .clear)
                .frame(width: 3, height: 28)

            ZStack {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(
                        isServing
                            ? GeminiPalette.emerald.opacity(colorScheme == .dark ? 0.18 : 0.14)
                            : (isSelected
                                ? accent.opacity(colorScheme == .dark ? 0.22 : 0.14)
                                : GeminiPalette.pillFill(for: colorScheme, hovered: isHovered))
                    )
                    .frame(width: 32, height: 32)
                Image(systemName: isServing ? "play.fill" : "applewatch")
                    .font(.system(size: isServing ? 11 : 13, weight: .semibold))
                    .foregroundStyle(isServing ? GeminiPalette.emerald : (isSelected ? accent : Color.secondary))
            }

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 5) {
                    Text(instance.record.name)
                        .font(.system(size: 13, weight: isSelected ? .semibold : .medium))
                        .foregroundStyle(.primary)
                        .lineLimit(1)
                    Spacer(minLength: 0)
                    if instance.runtime.isBusy {
                        ProgressView().controlSize(.mini)
                            .help(instance.runtime.isStopping ? "正在停止" : "正在处理")
                    } else if hasError {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 11))
                            .foregroundStyle(GeminiPalette.amber)
                            .help("此实例有未处理的错误；选中查看，或使用「重置实例」")
                            .accessibilityLabel("有错误")
                    } else if isRunning {
                        Circle()
                            .fill(GeminiPalette.emerald)
                            .frame(width: 6, height: 6)
                    }
                }
                Text(deviceTitle)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Text(instance.runtime.isStopping ? "正在停止" : instance.runtime.runtimeSession.stateLabel)
                    .font(.system(size: 10.5, weight: isRunning ? .medium : .regular))
                    .foregroundStyle(isRunning ? GeminiPalette.emerald : .secondary.opacity(0.85))
                    .lineLimit(1)
            }
        }
        .padding(.leading, 5)
        .padding(.trailing, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 11, style: .continuous)
                .fill(rowBackground)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 11, style: .continuous)
                .strokeBorder(rowBorder, lineWidth: 1)
        )
        .contentShape(Rectangle())
        .onTapGesture(perform: onSelect)
        .animation(.easeOut(duration: 0.14), value: isHovered)
        .animation(.easeOut(duration: 0.14), value: isSelected)
        .onHover { isHovered = $0 }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(isSelected ? [.isButton, .isSelected] : .isButton)
    }

    private var rowBackground: Color {
        if isSelected { return GeminiPalette.selectedRowFill(for: colorScheme) }
        if isHovered { return GeminiPalette.hoverRowFill(for: colorScheme) }
        return .clear
    }

    private var rowBorder: Color {
        if isSelected { return GeminiPalette.accent(for: colorScheme).opacity(colorScheme == .dark ? 0.36 : 0.24) }
        if isHovered { return GeminiPalette.hairlineBorder(for: colorScheme) }
        return .clear
    }
}

// MARK: - Gemini Welcome Empty Stage

@MainActor private struct GeminiEmptyWelcomeStage: View {
    let library: SimulatorInstanceLibrary
    let onCreate: () -> Void
    let onImport: () -> Void
    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        let accent = GeminiPalette.accent(for: colorScheme)
        ZStack {
            GeminiAtmosphereBackdrop(intensity: .hero, isActive: true)
            GeminiInteractiveMistOverlay(isActive: true)
            GeminiGlassCard(cornerRadius: 22, isSelected: false, hoverGlow: false, padding: 34) {
                VStack(spacing: 16) {
                    ZStack {
                        Circle()
                            .fill(accent.opacity(colorScheme == .dark ? 0.18 : 0.10))
                            .frame(width: 56, height: 56)
                            .overlay(
                                Circle()
                                    .strokeBorder(accent.opacity(0.32), lineWidth: 1)
                            )
                        Image(systemName: "applewatch")
                            .font(.system(size: 24, weight: .medium))
                            .foregroundStyle(accent)
                    }

                    VStack(spacing: 6) {
                        Text("创建你的第一台设备")
                            .font(.system(size: 20, weight: .semibold))
                        Group {
                            if !library.catalog.hasLoadedCatalog || library.catalog.isBusy {
                                Text("正在读取设备型号…")
                            } else if library.deviceModels.isEmpty {
                                Text(library.catalog.libraryStatus)
                            } else {
                                Text("选择硬件型号即可创建虚拟设备；可随时导入固件包并自由切换运行。")
                            }
                        }
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 360)
                    }

                    HStack(spacing: 10) {
                        Button {
                            onCreate()
                        } label: {
                            Label("新建设备…", systemImage: "plus")
                        }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .primary))
                        .disabled(SimulatorInstanceActions.canCreateReason(library) != nil)
                        .accessibilityIdentifier("simulator.create-instance.empty")

                        Button {
                            onImport()
                        } label: {
                            Label("导入固件…", systemImage: "square.and.arrow.down")
                        }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .secondary))

                        if library.catalog.hasLoadedCatalog && !library.catalog.isBusy && library.deviceModels.isEmpty {
                            Button("重新读取固件库") { Task { await library.catalog.loadCatalog() } }
                                .buttonStyle(GeminiNeonButtonStyle(tone: .subtle))
                        }
                    }
                    .padding(.top, 4)
                }
            }
            .padding(32)
        }
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
                switch result {
                case .success(let url):
                    if let instance = presentedInstance {
                        Task {
                            await instance.runtime.importFirmware(url)
                            await library.refreshAllCatalogs(excluding: instance.id)
                            if instance.runtime.importFailureMessage == nil, instance.runtime.errorMessage == nil {
                                library.showToast("固件导入完成")
                            }
                        }
                    } else {
                        Task {
                            await library.catalog.importFirmware(url)
                            await library.refreshAllCatalogs()
                            if library.catalog.importFailureMessage == nil, library.catalog.errorMessage == nil {
                                library.showToast("固件已导入至固件库")
                            }
                        }
                    }
                case .failure(let error):
                    if (error as NSError).code != NSUserCancelledError {
                        if let instance = presentedInstance {
                            instance.runtime.errorMessage = error.localizedDescription
                        } else {
                            library.errorMessage = error.localizedDescription
                        }
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
    @Binding var logsPresented: Bool
    @Binding var importFirmware: Bool

    func body(content: Content) -> some View {
        if let instance = library.selected {
            content.inspector(isPresented: $presented) {
                SimulatorInstanceInspector(
                    library: library,
                    instance: instance,
                    logsPresented: $logsPresented,
                    importFirmware: $importFirmware
                )
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
                SimulatorDeviceDetailView(
                    library: library,
                    instance: instance,
                    logsPresented: $logsPresented,
                    onImportFirmware: { importFirmware = true }
                )
                .inspector(isPresented: $inspectorPresented) {
                    SimulatorInstanceInspector(
                        library: library,
                        instance: instance,
                        logsPresented: $logsPresented,
                        importFirmware: $importFirmware
                    )
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
                        onCreate: { creating = true }, onImport: { importFirmware = true },
                        logsPresented: $logsPresented
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
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext
    @Binding var logsPresented: Bool
    var onImportFirmware: () -> Void
    @Environment(\.openWindow) private var openWindow
    @Environment(\.colorScheme) private var colorScheme
    private var displaySettings: DisplaySettings { DisplaySettings.shared }
    private var session: SimulatorSessionModel { instance.runtime.runtimeSession }
    @State private var preferredLogPaneHeight = SimulatorDeviceContentLayout.defaultLogPaneHeight
    @State private var dragStartHeight: CGFloat?

    var body: some View {
        GeometryReader { proxy in
            let availableHeight = proxy.size.height
            let logHeight = logsPresented
                ? SimulatorDeviceContentLayout.logPaneHeight(availableHeight: availableHeight,
                                                            preferred: preferredLogPaneHeight)
                : 0
            let dividerHeight: CGFloat = logsPresented ? SimulatorDeviceContentLayout.dividerHeight : 0
            let canvasHeight = logsPresented
                ? SimulatorDeviceContentLayout.canvasHeight(availableHeight: availableHeight, logPaneHeight: logHeight)
                : availableHeight
            VStack(spacing: 0) {
                stageView
                    .frame(width: proxy.size.width, height: max(0, canvasHeight), alignment: .top)
                if logsPresented {
                    logDivider(availableHeight: availableHeight)
                        .frame(width: proxy.size.width, height: dividerHeight)
                    SimulatorLogPanel(runtime: instance.runtime, stream: instance.logStream,
                                      instanceName: instance.record.name)
                        .frame(width: proxy.size.width, height: logHeight, alignment: .topLeading)
                        .clipped()
                }
            }
            .frame(width: proxy.size.width, height: availableHeight, alignment: .top)
        }
        .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
        .clipped()
        .onChange(of: logsPresented) { _, _ in dragStartHeight = nil }
    }

    private var stageView: some View {
        let active = instance.runtime.isServing || session.isRunning
        return ZStack {
            GeminiAtmosphereBackdrop(intensity: .stage, isActive: active)
            GeminiInteractiveMistOverlay(isActive: active)

            VStack(spacing: 0) {
                stageHeader
                    .padding(.horizontal, 16)
                    .padding(.top, 12)
                    .padding(.bottom, 6)

                ZStack {
                    GeminiDeviceStageAura(
                        isRunning: session.isRunning,
                        acceptsInput: session.canInput
                    )
                    canvas
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                stageQuickDock
                    .padding(.horizontal, 16)
                    .padding(.top, 4)
                    .padding(.bottom, 14)
            }

            if let toast = library.toastMessage {
                VStack {
                    GeminiToastBanner(message: toast, isError: library.toastIsError) {
                        library.dismissToast()
                    }
                    .padding(.top, 52)
                    Spacer()
                }
                .animation(.easeOut(duration: 0.18), value: library.toastMessage)
            }
        }
    }

    private var statusTone: GeminiStatusTone {
        if instance.recording.isRecording { return .recording }
        if instance.runtime.isStopping { return .warning }
        if session.isRunning { return .running }
        if instance.runtime.isBusy || session.isConnecting || session.isAttached { return .connecting }
        return .idle
    }

    private var statusText: String {
        if instance.recording.isRecording {
            let secs = Int(instance.recording.elapsedTime)
            return "录制中 \(secs / 60):\(String(format: "%02d", secs % 60))"
        }
        if instance.runtime.isStopping { return "正在停止" }
        if instance.runtime.isBusy && !session.isRunning { return "准备中" }
        return session.stateLabel
    }

    private var stageHeader: some View {
        HStack(spacing: 8) {
            GeminiStatusPill(text: statusText, tone: statusTone)
            if let firmware = instance.runtime.selectedFirmware {
                Text(firmware.displayName)
                    .font(.system(size: 11.5, weight: .medium))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(GeminiPalette.pillFill(for: colorScheme, hovered: false)))
                    .overlay(Capsule().strokeBorder(GeminiPalette.hairlineBorder(for: colorScheme), lineWidth: 1))
            }
            Spacer(minLength: 0)
            if instance.sharing.isSharing {
                GeminiStatusPill(text: "Studio 共享中", tone: .connecting, compact: true)
            }
        }
    }

    private var stageQuickDock: some View {
        let serving = instance.runtime.isServing
        let recording = instance.recording.isRecording
        return HStack(spacing: 8) {
            Button {
                SimulatorInstanceActions.run(instance, library: library, onNeedImport: onImportFirmware)
            } label: {
                Label(serving ? "重启" : "运行", systemImage: serving ? "arrow.clockwise" : "play.fill")
            }
            .buttonStyle(GeminiNeonButtonStyle(tone: serving ? .secondary : .primary, compact: true))
            .disabled(!SimulatorInstanceActions.canRun(instance))

            if SimulatorInstanceActions.canStop(instance) {
                Button {
                    SimulatorInstanceActions.stop(instance, library: library)
                } label: {
                    Label("停止", systemImage: "stop.fill")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .destructive, compact: true))
            }

            if SimulatorInstanceActions.canScreenshot(instance) {
                Button {
                    SimulatorInstanceActions.screenshot(instance, library: library)
                } label: {
                    Label("截图", systemImage: "camera")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
            }

            if SimulatorInstanceActions.canToggleRecording(instance) {
                Button {
                    SimulatorInstanceActions.toggleRecording(instance, library: library)
                } label: {
                    Label(recording ? "停止录屏" : "录屏",
                          systemImage: recording ? "stop.circle.fill" : "record.circle")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: recording ? .recording : .secondary, compact: true))
            }

            if SimulatorInstanceActions.watchButtonsAvailable(instance) {
                Button {
                    session.sendDeviceButton("crown")
                } label: {
                    Label("表冠", systemImage: "button.programmable")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))

                Button {
                    session.sendDeviceButton("lower")
                } label: {
                    Label("下键", systemImage: "hand.tap")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
            }

            Button {
                logsPresented.toggle()
            } label: {
                Label("日志", systemImage: "text.alignleft")
            }
            .buttonStyle(GeminiNeonButtonStyle(tone: logsPresented ? .active : .subtle, compact: true))
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(
            Capsule()
                .fill(GeminiPalette.cardFill(for: colorScheme, elevated: true))
        )
        .overlay(
            Capsule()
                .strokeBorder(GeminiPalette.hairlineBorder(for: colorScheme), lineWidth: 1)
        )
        .shadow(color: Color.black.opacity(colorScheme == .dark ? 0.24 : 0.07), radius: 10, y: 3)
    }

    /// A SwiftUI divider replaces the AppKit split view: dragging changes only
    /// the stored preference, never the measured container, so live log text
    /// can no longer move the canvas or push it under the window toolbar.
    private func logDivider(availableHeight: CGFloat) -> some View {
        GeminiNeonDividerHandle(isDragging: dragStartHeight != nil)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 1)
                .onChanged { value in
                    let start = dragStartHeight ?? preferredLogPaneHeight
                    if dragStartHeight == nil { dragStartHeight = start }
                    let proposed = start - value.translation.height
                    let upperBound = SimulatorDeviceContentLayout.maximumLogPaneHeight(availableHeight: availableHeight)
                    preferredLogPaneHeight = min(max(SimulatorDeviceContentLayout.minimumLogPaneHeight, proposed), upperBound)
                }
                .onEnded { _ in dragStartHeight = nil })
            .accessibilityIdentifier("simulator.log-divider")
    }

    @ViewBuilder private var canvas: some View {
        if let geometry = session.manifest?.displayGeometry {
            SimulatorNativeCanvas(
                image: session.image,
                geometry: geometry,
                acceptsInput: session.canInput,
                clipToScreenShape: displaySettings.clipToScreenShape,
                onInput: session.sendInput
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .accessibilityIdentifier("simulator.canvas.\(instance.id.uuidString)")
            .contextMenu {
                Button("截图") { SimulatorInstanceActions.screenshot(instance, library: library) }
                    .disabled(!SimulatorInstanceActions.canScreenshot(instance))
                Toggle("按真实屏幕形状裁切", isOn: Binding(
                    get: { displaySettings.clipToScreenShape },
                    set: { displaySettings.clipToScreenShape = $0 }
                ))
                Divider()
                Button("在新窗口打开") { openWindow(id: "device-display", value: instance.id) }
            }
        } else {
            let accent = GeminiPalette.accent(for: colorScheme)
            GeminiGlassCard(cornerRadius: 20, isActive: session.isAttached, hoverGlow: false, padding: 28) {
                VStack(spacing: 14) {
                    ZStack {
                        Circle()
                            .fill(accent.opacity(colorScheme == .dark ? 0.16 : 0.10))
                            .frame(width: 54, height: 54)
                        Image(systemName: session.isAttached ? "sparkles" : "applewatch")
                            .font(.system(size: 24, weight: .light))
                            .foregroundStyle(accent)
                    }
                    Text(session.isAttached ? "等待画面" : "设备准备就绪")
                        .font(.system(size: 16, weight: .semibold))
                    Text(
                        session.isAttached
                            ? "设备已连接，正在等待第一帧渲染。"
                            : (instance.runtime.selectableFirmwareItems.isEmpty
                                ? "点击下方「导入固件」添加固件包，或直接点「运行」快速引导。"
                                : "点击「运行」即可开机；也可在右侧检查器自由调整固件与硬件数据。")
                    )
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 320)

                    if !session.isAttached {
                        HStack(spacing: 10) {
                            Button {
                                SimulatorInstanceActions.run(instance, library: library, onNeedImport: onImportFirmware)
                            } label: {
                                Label("立即运行", systemImage: "play.fill")
                            }
                            .buttonStyle(GeminiNeonButtonStyle(tone: .primary))
                            .disabled(!SimulatorInstanceActions.canRun(instance))

                            Button {
                                onImportFirmware()
                            } label: {
                                Label("导入固件…", systemImage: "square.and.arrow.down")
                            }
                            .buttonStyle(GeminiNeonButtonStyle(tone: .secondary))
                        }
                        .padding(.top, 2)
                    }
                }
            }
            .frame(maxWidth: 420)
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
                        .font(.caption).foregroundStyle(.secondary)
                    TextField("蓝牙地址", text: $address, prompt: Text("02:12:34:56:78:9A"))
                        .accessibilityIdentifier("simulator.virtual-address")
                    HStack {
                        Button {
                            let generated = SimulatorVirtualIdentity.generated(for: UUID())
                            serial = generated.serial
                            address = generated.address
                            failureMessage = nil
                        } label: {
                            Label("随机生成身份", systemImage: "dice")
                        }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                        Spacer()
                    }
                    Text("供固件识别此虚拟设备；不改变 Mac 对外广播的物理地址或系统配对身份。")
                        .font(.caption).foregroundStyle(.secondary)
                    Text("下次启动使用新身份和全新设备数据，需要重新配对。原应用、表盘、设置及配对记录保留在旧快照中。身份未改变时不会重建数据。")
                        .font(.caption).foregroundStyle(.secondary)
                    if loading { ProgressView() }
                    if let failureMessage { Text(failureMessage).foregroundStyle(.red) }
            }
            .formStyle(.grouped)
            .navigationTitle("虚拟设备身份")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }
                        .keyboardShortcut(.cancelAction)
                }
                ToolbarItem(placement: .destructiveAction) {
                    Button("应用新身份并重建数据", role: .destructive) {
                        do {
                            try library.setVirtualIdentity(instance.id, identity: .init(serial: serial, address: address))
                            dismiss()
                        } catch { failureMessage = error.localizedDescription }
                    }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .destructive, compact: true))
                    .disabled(loading || serial.isEmpty || address.isEmpty)
                    .accessibilityIdentifier("simulator.save-virtual-identity")
                }
            }
        }
        .frame(minWidth: 480, minHeight: 380)
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
                if library.deviceModels.isEmpty {
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small)
                            Text("正在加载可用设备型号…").foregroundStyle(.secondary)
                        }
                    } else {
                        Picker("设备型号", selection: $newModel) {
                            ForEach(library.deviceModels, id: \.model) { item in
                                Text(SimulatorInstanceLibrary.deviceName(item)).tag(item.model)
                            }
                        }.accessibilityIdentifier("simulator.new-instance-model")
                    }
                    TextField("实例名称", text: Binding(get: { nameDraft.text }, set: { nameDraft.edit($0); failureMessage = nil }),
                              prompt: Text(nameDraft.defaultName))
                        .accessibilityIdentifier("simulator.new-instance-name")
                        .help("用于实例列表、设备标题和录屏文件名；不会修改固件型号或序列号")
                    TextField("虚拟序列号", text: $serial)
                        .accessibilityIdentifier("simulator.new-instance-serial")
                    TextField("虚拟蓝牙地址", text: $address)
                        .accessibilityIdentifier("simulator.new-instance-address")
                    Text("序列号供固件和应用识别；虚拟地址属于模拟设备，不改变 Mac 的物理蓝牙身份。")
                        .font(.caption).foregroundStyle(.secondary)
                    Toggle("跳过首次设置（OOBE）", isOn: $skipOOBE)
                        .accessibilityIdentifier("simulator.new-instance-skip-oobe")
                    Text(skipOOBE ? "首次启动直接进入设备界面。" : "保留语言选择和配对引导，可开启外部蓝牙后自行连接。")
                        .font(.caption).foregroundStyle(.secondary)
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
                    .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
                    .disabled(selectedModel == nil || nameDraft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .accessibilityIdentifier("simulator.confirm-create-instance")
                    .keyboardShortcut(.defaultAction)
                }
            }
            .task {
                if !library.catalog.hasLoadedCatalog { await library.catalog.loadCatalog() }
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
                    .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
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
