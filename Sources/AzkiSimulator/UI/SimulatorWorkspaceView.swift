import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor struct SimulatorWorkspaceView: View {
    enum Presentation { case full, controlsAndLogs }
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    let presentation: Presentation
    let isActive: Bool
    let instanceName: String?
    let instanceRecording: SimulatorRecordingModel?
    @State private var localRecording = SimulatorRecordingModel()
    @State private var choosingFile = false
    @State private var fileChoice: SimulatorFileChoice = .session
    @State private var showingAdvanced = false
    @State private var showingLogs = false
    @State private var logStream = SimulatorLogStreamModel()
    @State private var displayWindow = SimulatorDisplayWindow()
    @State private var guestOperations = SimulatorGuestOperationsModel()
    @State private var showingPreparedFiles = false
    @State private var showingSharing = false
    @State private var review: SimulatorRunReview?
    @State private var uiError: String?
    @State private var debugText = ""
    @State private var debugDocument = SimulatorDebugDocument(data: Data())
    @State private var exportingDebug = false
    @State private var adcDraft = PMUADCDraft()
    @State private var editingFirmwareSettings = false

    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var recording: SimulatorRecordingModel { instanceRecording ?? localRecording }
    private var controlsAvailable: Bool { Self.coreControlsAvailable(runtime: runtime, sharing: sharing) }
    private var canEnableSharing: Bool {
        Self.canEnableSharing(runtime: runtime, sharing: sharing)
    }
    private var allFilesSelected: Bool { runtime.qemuURL != nil && runtime.firmwareURL != nil && runtime.resourceURL != nil }
    private var displayGeometry: SimulatorDisplayGeometry? { session.manifest?.displayGeometry }
    private var logsVisible: Bool { presentation == .controlsAndLogs || showingLogs || displayWindow.isDetached }
    private var guestAvailable: Bool {
        session.isRunning && session.status?["guestConnectSupported"] != .bool(false)
    }
    private var guestContext: String { "\(session.manifest?.id.uuidString ?? "none")/\(guestAvailable)" }
    private var adcAvailable: Bool {
        guard session.isRunning, case .array(let inputs)? = session.status?["hardwareInputs"] else { return false }
        return inputs.contains(.string("pmu.adcRaw"))
    }
    private var parsedADC: [Int]? {
        adcDraft.values
    }
    private var adcProfileID: String? {
        guard case .string(let profile)? = session.status?["profileID"] else { return nil }
        return profile
    }

    init(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel,
         presentation: Presentation = .full, isActive: Bool = true, instanceName: String? = nil,
         recording: SimulatorRecordingModel? = nil) {
        self.runtime = runtime
        self.sharing = sharing
        self.presentation = presentation
        self.isActive = isActive
        self.instanceName = instanceName
        self.instanceRecording = recording
    }

    init(runtime: StandaloneRuntimeModel) {
        self.init(runtime: runtime, sharing: HostStatusSharingModel())
    }

    static func canEnableSharing(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel) -> Bool {
        !runtime.isBusy && !runtime.isServing && !runtime.runtimeSession.isAttached
            && !runtime.runtimeSession.isCommandInFlight && !sharing.isSharing && !sharing.isBusy && sharing.sessionURL == nil
    }

    static func coreControlsAvailable(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel) -> Bool {
        !runtime.isBusy && !runtime.runtimeSession.isCommandInFlight && !sharing.isSharing && !sharing.isBusy && sharing.sessionURL == nil
    }

    static func canStopSharing(_ sharing: HostStatusSharingModel) -> Bool {
        (sharing.isSharing || sharing.sessionURL != nil) && !sharing.isBusy
    }

    var body: some View {
        ZStack {
            SimulatorTheme.canvas.ignoresSafeArea()
            if presentation == .controlsAndLogs {
                VSplitView {
                    ScrollView {
                        VStack(alignment: .leading, spacing: SimulatorTheme.inset) {
                            header
                            controlRegions
                        }.padding(16).frame(maxWidth: .infinity, alignment: .topLeading)
                    }.frame(minHeight: 260)
                    logPanel.frame(minHeight: 180, idealHeight: 240)
                }
            } else { fullPresentation }
        }
        .frame(minWidth: presentation == .full ? 760 : 420, minHeight: 620)
        .font(.system(size: 14))
        .foregroundStyle(SimulatorTheme.text)
        .tint(SimulatorTheme.accent)
        .buttonStyle(.bordered)
        .controlSize(.large)
        .fileImporter(isPresented: $choosingFile, allowedContentTypes: fileChoice == .session ? [.folder] : [.item]) { result in
            switch result {
            case .success(let url): selected(url, for: fileChoice)
            case .failure(let error):
                if (error as NSError).code != NSUserCancelledError { uiError = error.localizedDescription }
            }
        }
        .fileExporter(isPresented: $exportingDebug, document: debugDocument, contentType: .json,
                      defaultFilename: "Azki-debug-\(session.manifest?.id.uuidString.prefix(8) ?? "session")") { result in
            if case .failure(let error) = result, (error as NSError).code != NSUserCancelledError {
                uiError = error.localizedDescription
            }
        }
        .sheet(item: Binding(get: { isActive ? review : nil }, set: { if isActive { review = $0 } })) { prepared in
            SimulatorRunConfirmation(summary: prepared.summary, runLimitSeconds: prepared.runLimitSeconds) {
                review = nil
            } confirm: {
                review = nil
                guard controlsAvailable, runtime.canStartPrepared, runtime.selectedSummary == prepared.summary,
                      runtime.preparedRunLimitSeconds == prepared.runLimitSeconds else {
                    uiError = "准备的文件已变化，请重新准备并核对指纹。"
                    return
                }
                Task { await runtime.startApproved() }
            }
        }
        .sheet(isPresented: Binding(get: { isActive && editingFirmwareSettings }, set: { if isActive { editingFirmwareSettings = $0 } })) {
            SimulatorFirmwareSettingsSheet(runtime: runtime, restartAvailable: runtime.isServing) {
                Task { await recording.stop(); await runtime.coldRestart() }
            }
        }
        .task {
            if !runtime.hasLoadedCatalog && controlsAvailable { await runtime.loadCatalog() }
        }
        .onChange(of: runtime.resetEpoch) { _, _ in
            // The instance was reset or is booting again: drop this view's
            // per-instance caches so nothing from the previous run lingers.
            debugText = ""; uiError = nil; review = nil
            adcDraft = PMUADCDraft()
            guestOperations.invalidate()
            logStream.clear()
        }
        .task(id: guestContext) {
            guestOperations.configure(hostSessionID: session.manifest?.id, available: guestAvailable,
                packagesAvailable: session.status?["guestInstallSupported"] != .bool(false),
                command: { try await session.guestCommand($0, arguments: $1) },
                install: { try await session.installGuestPackage($0, expectedSessionID: $1, expectedGeneration: $2) })
            // This task deliberately does not depend on which instance is selected.
            while !Task.isCancelled, guestOperations.isAvailable {
                await guestOperations.refresh()
                do { try await Task.sleep(for: .milliseconds(750)) }
                catch { break }
            }
        }
        .task(id: "\(session.manifest?.id.uuidString ?? "none")/\(logsVisible)/\(logStream.selectedName)") {
            // Keep each instance's bounded stream/cursor alive while its view is
            // hidden, so changing controls never resets another instance's log.
            if logsVisible, let id = session.manifest?.id, let directory = runtime.sessionURL {
                logStream.start(directory: directory, sessionID: id, name: logStream.selectedName)
            } else { logStream.stop() }
        }
        .onDisappear {
            logStream.stop(); displayWindow.close()
            guestOperations.invalidate()
            Task { await recording.stop() }
        }
        .alert(instanceName.map { "Azki Simulator · \($0)" } ?? "Azki Simulator",
               isPresented: Binding(get: { isActive && errorMessage != nil }, set: { if !$0 && isActive { dismissErrors() } })) {
            Button("确定") { dismissErrors() }
        } message: { Text(errorMessage ?? "") }
    }

    private var fullPresentation: some View {
        GeometryReader { viewport in
            ScrollView {
                VStack(alignment: .leading, spacing: SimulatorTheme.inset) {
                    header
                    if !displayWindow.isDetached, let geometry = displayGeometry {
                        let paneWidth = geometry.size.width + SimulatorTheme.inset * 2
                        if viewport.size.width >= paneWidth + SimulatorTheme.minimumSectionWidth + SimulatorTheme.gap + SimulatorTheme.pageInset * 2 {
                            HStack(alignment: .top, spacing: SimulatorTheme.gap) {
                                devicePane(geometry).frame(width: paneWidth)
                                controlRegions
                            }
                        } else {
                            devicePane(geometry).frame(width: paneWidth)
                            controlRegions
                        }
                    } else { controlRegions }
                }
                .padding(SimulatorTheme.pageInset)
                .frame(maxWidth: .infinity, alignment: .topLeading)
            }
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: SimulatorTheme.gap) {
            Text(instanceName ?? "Azki Simulator")
                .font(.system(size: presentation == .full ? SimulatorTheme.titleSize : 24, design: .serif))
                .accessibilityAddTraits(.isHeader)
            Spacer()
            Text(session.stateLabel)
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(SimulatorTheme.secondary)
                .accessibilityIdentifier("simulator.state")
        }
    }

    private var controlRegions: some View {
        VStack(spacing: 0) {
            SimulatorDeviceControls(runtime: runtime, sharing: sharing, recording: recording,
                guestOperations: guestOperations, instanceName: instanceName) {
                if presentation == .full {
                    action("日志", icon: "text.alignleft", enabled: controlsAvailable && session.isAttached) {
                        showingLogs.toggle()
                    }
                }
                action(displayWindow.isDetached ? "显示独立窗口" : "独立显示窗口", icon: "macwindow",
                       enabled: runtime.instanceID != nil || session.isAttached) {
                    displayWindow.show(runtime: runtime, sharing: sharing, recording: recording,
                        guestOperations: guestOperations, logStream: logStream, instanceName: instanceName)
                }
                .accessibilityIdentifier("simulator.open-display-window")
                if displayWindow.isDetached {
                    action(presentation == .full ? "画面回到主窗口" : "关闭独立窗口", icon: "rectangle.inset.filled", enabled: true) {
                        displayWindow.close()
                    }
                }
            }
            SimulatorSectionDivider()
            if runtime.instanceID != nil {
                SimulatorSection(title: "设备数据", icon: "sensor") {
                    SimulatorDeviceDataView(runtime: runtime, session: session, enabled: controlsAvailable) {
                        editingFirmwareSettings = true
                    }
                    SimulatorActionFlow {
                        action("冷重启", icon: "arrow.clockwise.circle", enabled: controlsAvailable && runtime.isServing) {
                            Task { await recording.stop(); await runtime.coldRestart() }
                        }
                        .accessibilityIdentifier("simulator.cold-restart")
                    }
                }
                SimulatorSectionDivider()
            }
            DisclosureGroup(isExpanded: $showingSharing) {
                SimulatorActionFlow {
                    action("启用 Studio 连接", icon: "link", enabled: canEnableSharing) {
                        Task {
                            guard canEnableSharing else { return }
                            await sharing.enable()
                        }
                    }
                    .accessibilityIdentifier("simulator.enable-status-sharing")
                    action("停止共享", icon: "eject", enabled: Self.canStopSharing(sharing)) {
                        Task { await sharing.disable() }
                    }
                    .accessibilityIdentifier("simulator.disable-status-sharing")
                }
                Text(sharing.isSharing ? "Studio 可扫描连接并推送表盘" : sharing.sessionURL != nil ? "目录清理待重试" : "启用后，Studio 会自动扫描；扫描失败时可选择共享目录。")
                    .foregroundStyle(SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let url = sharing.sessionURL {
                    Text(url.path).font(.system(size: 12, design: .monospaced))
                        .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("simulator.status-sharing-directory")
                }
                Text("支持状态、日志与表盘文件接收；尚不支持客体安装运行。接收文件随共享停止清理。共享期间不能启动核心。")
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if sharing.isBusy { ProgressView().controlSize(.small).accessibilityLabel("正在更新 Studio 连接") }
            } label: {
                Label("Studio 连接", systemImage: "folder").font(.headline)
            }.padding(SimulatorTheme.inset)
                .help("只读状态共享不支持虚拟设备发现或配对")
            SimulatorSectionDivider()
            SimulatorSection(title: "高级调试", icon: "slider.horizontal.3") {
                SimulatorActionFlow {
                    action("采集调试信息", icon: "waveform.path.ecg", enabled: session.isAttached) { captureDebug(export: false) }
                        .accessibilityIdentifier("simulator.capture-debug")
                    action("导出调试快照…", icon: "square.and.arrow.up", enabled: session.isAttached) { captureDebug(export: true) }
                }
                if !debugText.isEmpty {
                    Text(debugText).font(.system(size: 12, design: .monospaced))
                        .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                }
                DisclosureGroup("会话与运行文件", isExpanded: $showingAdvanced) {
                    VStack(alignment: .leading, spacing: SimulatorTheme.inset) {
                        if let manifest = session.manifest {
                            Text("会话 \(manifest.id.uuidString.lowercased())")
                                .font(.system(size: 12, design: .monospaced)).textSelection(.enabled)
                        }
                        action("连接已有会话", icon: "folder", enabled: controlsAvailable) { choose(.session) }
                            .accessibilityIdentifier("simulator.connect")
                        fileRow("QEMU 核心", url: runtime.qemuURL, choice: .qemu)
                        fileRow("系统固件", url: runtime.firmwareURL, choice: .firmware)
                        fileRow("资源固件", url: runtime.resourceURL, choice: .resource)
                        SimulatorActionFlow {
                            action("准备会话", icon: "checkmark.shield", enabled: allFilesSelected && controlsAvailable && !runtime.isServing) {
                                Task { await runtime.prepare() }
                            }
                            .accessibilityIdentifier("simulator.prepare")
                            action("核对并启动", icon: "play.fill", enabled: runtime.canStartPrepared && controlsAvailable) {
                                review = SimulatorRunReview(summary: runtime.selectedSummary, runLimitSeconds: runtime.preparedRunLimitSeconds)
                            }
                            .accessibilityIdentifier("simulator.review-start")
                        }
                        if runtime.isBusy { ProgressView().controlSize(.small).accessibilityLabel("正在处理会话") }
                        if runtime.sessionURL != nil {
                            DisclosureGroup("来源与 SHA-256", isExpanded: $showingPreparedFiles) {
                                Text(runtime.selectedSummary)
                                    .font(.system(size: 12, design: .monospaced))
                                    .textSelection(.enabled)
                                    .fixedSize(horizontal: false, vertical: true)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                            }
                        }
                    }
                }
            }
            SimulatorSectionDivider()
            SimulatorSection(title: "PMU 原始采样（调试）", icon: "waveform.path.ecg") {
                Text("按原始计数直接改写 PMU 的八路采样，用于调试固件的电压与温度通路。日常修改电量、充电、温度请使用上方“设备数据”。")
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                if adcAvailable {
                    Group {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("这些输入改变固件从 PMU 读取的原始计数。范围 0…65535，启动默认值 21000；重启后恢复。读取次数可用于确认固件是否取用了输入，不代表电量或温度变化。")
                                .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                            ForEach(PMUADCDescriptors.channels, id: \.self) { index in
                                VStack(alignment: .leading, spacing: 3) {
                                    HStack {
                                        Text(PMUADCDescriptors.title(index, profileID: adcProfileID))
                                        Spacer(minLength: 8)
                                        TextField("先读取", text: $adcDraft.text[index])
                                            .textFieldStyle(.roundedBorder).frame(width: 90)
                                            .accessibilityLabel("\(PMUADCDescriptors.title(index, profileID: adcProfileID))，未校准原始值")
                                            .accessibilityIdentifier("simulator.adc.\(index)")
                                    }
                                    Text(PMUADCDescriptors.purpose(index, profileID: adcProfileID))
                                        .font(.system(size: 11)).foregroundStyle(SimulatorTheme.secondary)
                                    Text(PMUADCDescriptors.readCount(index, response: session.adcState))
                                        .font(.system(size: 11)).foregroundStyle(SimulatorTheme.secondary)
                                    Text(PMUADCDescriptors.readStatus(index, response: session.adcState))
                                        .font(.system(size: 11)).foregroundStyle(SimulatorTheme.secondary)
                                }
                            }
                            SimulatorActionFlow {
                                action("读取当前采样", icon: "arrow.clockwise", enabled: controlsAvailable) { session.perform("pmu-adc-get") }
                                    .accessibilityIdentifier("simulator.adc.read")
                                action("应用原始输入", icon: "slider.horizontal.3", enabled: controlsAvailable && parsedADC != nil && adcDraft.hasChanges) {
                                    guard let values = parsedADC else { return }
                                    session.perform("pmu-adc-set", arguments: ["values": .array(values.map { .number(Double($0)) })])
                                }.accessibilityIdentifier("simulator.adc.apply")
                                action("恢复启动默认值", icon: "arrow.counterclockwise", enabled: controlsAvailable && adcDraft.observed != nil) {
                                    adcDraft.useBootDefaults()
                                }.help("将八路输入填为 21000，点击应用后生效")
                                action("撤销修改", icon: "arrow.uturn.backward", enabled: controlsAvailable && adcDraft.hasChanges) {
                                    adcDraft.discardEdits()
                                }
                            }
                            if adcDraft.hasChanges {
                                Text(parsedADC == nil ? "每项需要输入 0…65535 之间的整数。" : "有尚未应用的修改；刷新读取状态会保留这些修改。")
                                    .font(.caption).foregroundStyle(SimulatorTheme.secondary)
                            }
                            if let state = session.adcState {
                                DisclosureGroup("采样详情") {
                                    Text(state.prettyDescription).font(.system(size: 12, design: .monospaced))
                                        .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                                }
                            }
                        }
                        .onChange(of: session.adcState, initial: true) { _, response in
                            adcDraft.receive(response)
                        }
                    }
                } else {
                    Text(session.isRunning ? "当前核心不支持 ADC 输入" : "运行后可编辑支持的原始输入")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                }
            }
            SimulatorSectionDivider()
            if presentation == .full, showingLogs {
                SimulatorSectionDivider()
                logPanel.frame(height: 340)
            }
        }
        .simulatorSurface()
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }

    private var logPanel: some View {
        SimulatorLogPanel(runtime: runtime, stream: logStream, instanceName: instanceName, allowsReconnect: isActive)
    }

    private func devicePane(_ geometry: SimulatorDisplayGeometry) -> some View {
        SimulatorSection(title: "设备画面", icon: "applewatch") {
            SimulatorNativeCanvas(image: session.image, geometry: geometry, acceptsInput: session.canInput,
                                  onInput: session.sendInput, onDeviceButton: { session.sendDeviceButton($0) })
                .frame(width: geometry.size.width, height: geometry.size.height)
                .accessibilityIdentifier("simulator.canvas")
        }
        .simulatorSurface()
    }

    private func fileRow(_ title: String, url: URL?, choice: SimulatorFileChoice) -> some View {
        VStack(alignment: .leading, spacing: SimulatorTheme.gap / 2) {
            HStack(spacing: SimulatorTheme.gap) {
                Text(title).fontWeight(.medium)
                Spacer(minLength: SimulatorTheme.gap)
                action("选择…", icon: "folder", enabled: controlsAvailable && !runtime.isServing) { choose(choice) }
            }
            Text(url?.path ?? "尚未选择")
                .font(.system(size: 12))
                .foregroundStyle(SimulatorTheme.secondary)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func action(_ title: String, icon: String, enabled: Bool, run: @escaping () -> Void) -> some View {
        Button(action: run) { Label(title, systemImage: icon).simulatorActionLabel() }
            .disabled(!enabled)
    }
    private func captureDebug(export: Bool) {
        let snapshot = SimulatorDebugSnapshot(manifest: session.manifest, status: session.status,
            frame: session.frame, isAttached: session.isAttached, logName: logsVisible ? logStream.selectedName : nil,
            logText: logsVisible ? logStream.text : nil)
        let frame = snapshot.frame.map { "\($0.width) × \($0.height) · 序号 \($0.sequence)" } ?? "尚无完整帧"
        debugText = "固件：\(snapshot.profileID ?? "未报告")\n状态：\(snapshot.reportedState ?? "未报告")\n画面：\(frame)\n核心进程：\(snapshot.hostProcessRunning.map { $0 ? "运行" : "停止" } ?? "未报告")\n客体安装：\(snapshot.guestInstallSupported.map { $0 ? "后端声明支持" : "未实现" } ?? "未报告")\n采集时间：\(snapshot.capturedAt.formatted())"
        if export {
            do { debugDocument = SimulatorDebugDocument(data: try snapshot.encodedJSON()); exportingDebug = true }
            catch { uiError = error.localizedDescription }
        }
    }
    private func choose(_ choice: SimulatorFileChoice) { fileChoice = choice; choosingFile = true }
    private func selected(_ url: URL, for choice: SimulatorFileChoice) {
        guard controlsAvailable else { uiError = "请先停止只读共享或等待当前操作结束。"; return }
        switch choice {
        case .qemu: runtime.qemuURL = url
        case .firmware: runtime.firmwareURL = url
        case .resource: runtime.resourceURL = url
        case .session: Task { guard controlsAvailable else { return }; await runtime.attachExisting(url) }
        }
    }
    private var errorMessage: String? { uiError ?? sharing.errorMessage ?? runtime.errorMessage ?? session.errorMessage }
    private func dismissErrors() { uiError = nil; sharing.errorMessage = nil; runtime.errorMessage = nil; session.errorMessage = nil }
}

private enum SimulatorFileChoice { case qemu, firmware, resource, session }
struct SimulatorRunReview: Identifiable {
    let id = UUID()
    let summary: String
    let runLimitSeconds: Int
}

struct SimulatorRunConfirmation: View {
    let summary: String
    let runLimitSeconds: Int
    let cancel: () -> Void
    let confirm: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: SimulatorTheme.inset) {
            Text("确认运行所选核心与固件")
                .font(.system(size: 24, weight: .semibold))
            Text("将运行以下核心与固件，\(runLimitSeconds == 0 ? "直到手动停止" : "本次最长 " + StandaloneRunLimits.describe(runLimitSeconds))。请核对文件来源与指纹；未知来源程序可能存在缺陷或恶意行为。")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(SimulatorTheme.secondary)
            ScrollView {
                Text(summary)
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(minHeight: 200, maxHeight: 340)
            HStack {
                Spacer()
                Button("取消", action: cancel).keyboardShortcut(.cancelAction)
                Button(runLimitSeconds == 0 ? "确认运行" : "确认运行（最长 \(StandaloneRunLimits.describe(runLimitSeconds))）", action: confirm).keyboardShortcut(.defaultAction)
            }
            .buttonStyle(.bordered)
            .controlSize(.large)
        }
        .padding(SimulatorTheme.pageInset)
        .frame(width: 640)
        .foregroundStyle(SimulatorTheme.text)
        .background(SimulatorTheme.canvas)
    }
}
