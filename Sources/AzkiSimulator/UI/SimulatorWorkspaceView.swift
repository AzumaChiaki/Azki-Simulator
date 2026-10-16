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
    @State private var fileChoice: SimulatorFileChoice = .importFirmware
    @State private var showingAdvanced = false
    @State private var showingLogs = false
    @State private var logName = "uart1.log"
    @State private var logStream = SimulatorLogStreamModel()
    @State private var followLogBottom = true
    @State private var displayWindow = SimulatorDisplayWindow()
    @State private var showingPackageReport = true
    @State private var guestOperations = SimulatorGuestOperationsModel()
    @State private var installSelectionHost: UUID?
    @State private var installSelectionPeer: SimulatorGuestOperationsModel.Identity?
    @State private var showingPreparedFiles = false
    @State private var showingSharing = false
    @State private var review: SimulatorRunReview?
    @State private var uiError: String?
    @State private var debugText = ""
    @State private var debugDocument = SimulatorDebugDocument(data: Data())
    @State private var exportingDebug = false
    @State private var adcDraft = PMUADCDraft()
    @State private var connectionTimeoutText = ""
    @State private var editingFirmwareSettings = false
    @State private var screenshotURL: URL?
    @State private var screenshotError: String?
    /// Firmware chosen while the device runs; applied by "切换并重新开机".
    @State private var pendingProfileID: String?

    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var recording: SimulatorRecordingModel { instanceRecording ?? localRecording }
    private var controlsAvailable: Bool { Self.coreControlsAvailable(runtime: runtime, sharing: sharing) }
    private var canEnableSharing: Bool {
        Self.canEnableSharing(runtime: runtime, sharing: sharing)
    }
    /// Stop stays available while starting or checking: it cancels that work
    /// instead of making the user wait for a stuck start to time out.
    private var canStop: Bool {
        !runtime.isStopping && (runtime.isServing || runtime.isBusy || (session.isRunning && !session.isCommandInFlight))
    }
    private var allFilesSelected: Bool { runtime.qemuURL != nil && runtime.firmwareURL != nil && runtime.resourceURL != nil }
    private var displayGeometry: SimulatorDisplayGeometry? { session.manifest?.displayGeometry }
    private var logsVisible: Bool { presentation == .controlsAndLogs || showingLogs }
    private var guestAvailable: Bool { session.isRunning && session.status?["guestInstallSupported"]?.bool == true }
    private var guestContext: String { "\(session.manifest?.id.uuidString ?? "none")/\(guestAvailable)" }
    private var sessionRunLimit: Int? {
        guard let value = session.status?["maxRuntimeSeconds"], case .number(let seconds) = value,
              seconds.isFinite, seconds.rounded() == seconds, (1...Double(StandaloneRunLimits.maximumSeconds)).contains(seconds) else { return nil }
        return Int(seconds)
    }
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
            connectionTimeoutText = String(runtime.connectionSettings.timeoutSeconds)
            if !runtime.hasLoadedCatalog && controlsAvailable { await runtime.loadCatalog() }
        }
        .onChange(of: runtime.connectionSettings.timeoutSeconds) { _, seconds in
            connectionTimeoutText = String(seconds)
        }
        .onChange(of: runtime.isServing) { _, serving in if !serving { pendingProfileID = nil } }
        .onChange(of: runtime.resetEpoch) { _, _ in
            // The instance was reset or is booting again: drop this view's
            // per-instance caches so nothing from the previous run lingers.
            debugText = ""; uiError = nil; review = nil; pendingProfileID = nil
            adcDraft = PMUADCDraft()
            guestOperations.invalidate()
            logStream.clear()
            showingPackageReport = true
        }
        .task(id: guestContext) {
            guestOperations.configure(hostSessionID: session.manifest?.id, available: guestAvailable,
                command: { try await session.guestCommand($0, arguments: $1) },
                install: { try await session.installGuestPackage($0, expectedSessionID: $1, expectedGeneration: $2) })
            // This task deliberately does not depend on which instance is selected.
            while !Task.isCancelled, guestOperations.isAvailable {
                await guestOperations.refresh()
                do { try await Task.sleep(for: .milliseconds(750)) }
                catch { break }
            }
        }
        .task(id: "\(session.manifest?.id.uuidString ?? "none")/\(logsVisible)/\(logName)") {
            // Keep each instance's bounded stream/cursor alive while its view is
            // hidden, so changing controls never resets another instance's log.
            if logsVisible, let id = session.manifest?.id, let directory = runtime.sessionURL {
                logStream.start(directory: directory, sessionID: id, name: logName)
            } else { logStream.stop() }
        }
        .onChange(of: session.manifest?.id) { _, id in displayWindow.reconcile(sessionID: id) }
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
            firmwareLibrarySection
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
            SimulatorSection(title: "会话", icon: "desktopcomputer") {
                HStack {
                    Text("连接超时（秒）")
                    TextField("30", text: $connectionTimeoutText)
                        .textFieldStyle(.roundedBorder).frame(width: 80)
                        .accessibilityIdentifier("simulator.connection-timeout")
                        .onChange(of: connectionTimeoutText) { _, value in
                            if let seconds = Int(value), ConnectionSettings.timeoutRange.contains(seconds) {
                                runtime.connectionSettings.timeoutSeconds = seconds
                            }
                        }
                    Text("1–300，自动保存")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                }
                Text("用于虚拟设备连接和通信请求，下次操作生效；不限制设备运行时长，硬件采样读取使用独立超时。")
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                if !connectionTimeoutText.isEmpty,
                   Int(connectionTimeoutText).map({ ConnectionSettings.timeoutRange.contains($0) }) != true {
                    Text("请输入 1–300 之间的整数，当前仍使用 \(runtime.connectionSettings.timeoutSeconds) 秒。")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                }
                SimulatorActionFlow {
                    if !runtime.isServing {
                        action("断开", icon: "eject", enabled: session.isAttached && controlsAvailable) { session.detach() }
                    }
                    action("停止模拟器", icon: "stop.fill", enabled: canStop) {
                        Task {
                            await recording.stop()
                            if runtime.isServing || runtime.isBusy { await runtime.stop() }
                            else { session.perform("stop") }
                        }
                    }
                    .keyboardShortcut(".", modifiers: .command)
                    .help(runtime.isStopping ? "正在停止…" : "停止设备（⌘.）；启动或检查进行中时会取消该操作")
                    .accessibilityIdentifier("simulator.stop")
                    if runtime.instanceID != nil {
                        action("重置实例", icon: "arrow.counterclockwise.circle", enabled: !runtime.isStopping) {
                            Task {
                                await recording.stop()
                                _ = await sharing.disable()
                                await runtime.resetInstance()
                            }
                        }
                        .help("启动失败或状态异常时使用：停止设备并清除此实例的运行缓存与错误；已保存的数据、固件设置和已导入固件都会保留")
                        .accessibilityIdentifier("simulator.reset-instance")
                    }
                    action("截图", icon: "camera", enabled: controlsAvailable && session.image != nil) {
                        guard let image = session.image else { return }
                        do {
                            screenshotURL = try SimulatorScreenshot.save(image, instanceName: instanceName ?? "Azki Simulator")
                            screenshotError = nil
                        } catch { screenshotError = error.localizedDescription }
                    }
                    action(recording.isRecording ? "停止录屏" : "录屏", icon: recording.isRecording ? "stop.circle" : "record.circle",
                           enabled: !recording.isFinishing && (recording.isRecording || (session.isRunning && session.image != nil))) {
                        if recording.isRecording { Task { await recording.stop() } }
                        else { recording.start(session: session, instanceName: instanceName ?? "Azki Simulator") }
                    }
                    .accessibilityIdentifier("simulator.recording")
                    .help("录制此设备的画面，完成后保存到桌面")
                    if presentation == .full {
                        action("日志", icon: "text.alignleft", enabled: controlsAvailable && session.isAttached) {
                            showingLogs.toggle()
                        }
                    }
                    action(displayWindow.isDetached ? "显示独立窗口" : "独立显示窗口", icon: "macwindow", enabled: session.isAttached) {
                        displayWindow.show(session: session)
                    }
                    if displayWindow.isDetached {
                        action(presentation == .full ? "画面回到主窗口" : "关闭独立窗口", icon: "rectangle.inset.filled", enabled: true) { displayWindow.close() }
                    }
                }
                if runtime.isStopping {
                    ProgressView("正在停止设备…").controlSize(.small)
                } else if !session.isRunning && !runtime.statusMessage.isEmpty {
                    Text(runtime.statusMessage)
                        .foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let issue = session.transientIssue {
                    Label(issue, systemImage: "exclamationmark.triangle")
                        .font(.system(size: 12)).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("simulator.transient-issue")
                }
                if let seconds = sessionRunLimit {
                    Text("运行上限：\(StandaloneRunLimits.describe(seconds))")
                        .foregroundStyle(SimulatorTheme.secondary)
                }
                if session.isAttached && session.image == nil {
                    Text("等待画面")
                        .foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let url = screenshotURL {
                    HStack {
                        Text("截图已保存到桌面：\(url.lastPathComponent)")
                            .foregroundStyle(SimulatorTheme.secondary)
                            .textSelection(.enabled)
                        Button("在访达中显示") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                    }
                }
                if let screenshotError {
                    Text(screenshotError).foregroundStyle(.red)
                }
                if recording.isRecording {
                    Label("录制中 \(Int(recording.elapsedTime) / 60):\(String(format: "%02d", Int(recording.elapsedTime) % 60))", systemImage: "record.circle.fill")
                        .foregroundStyle(.red)
                } else if recording.isFinishing {
                    ProgressView("正在保存录屏…").controlSize(.small)
                } else if let url = recording.savedFileURL {
                    Button("在桌面显示录屏") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                        .help(url.lastPathComponent)
                }
                if let message = recording.errorMessage {
                    Text(message).foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    if let url = recording.recoveryFileURL {
                        Button("显示可恢复视频") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                    }
                }
            }
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
            SimulatorSection(title: "应用与表盘", icon: "shippingbox") {
                SimulatorActionFlow {
                    action("连接虚拟手环", icon: "link", enabled: guestOperations.canConnect) {
                        Task { await guestOperations.connect() }
                    }
                    .help("连接此实例的本机虚拟通信通道")
                    .accessibilityIdentifier("simulator.guest.connect")
                    action("安装…", icon: "square.and.arrow.down", enabled: guestOperations.canInstall) {
                        installSelectionHost = session.manifest?.id
                        installSelectionPeer = guestOperations.identity
                        choose(.installGuestPackage)
                    }
                        .accessibilityIdentifier("simulator.install")
                }
                Text(session.isRunning ? guestOperations.connectionLabel : "运行设备后可连接")
                    .font(.system(size: 12)).accessibilityIdentifier("simulator.guest.connection-state")
                    .foregroundStyle(SimulatorTheme.secondary)
                if guestOperations.isAuthenticated {
                    HStack {
                        Picker("表盘", selection: $guestOperations.selectedWatchfaceID) {
                            Text(guestOperations.inventoryLoaded ? "选择表盘" : "正在读取…").tag(Optional<String>.none)
                            ForEach(guestOperations.inventory) { face in
                                Text("\(face.name)\(face.isCurrent ? " · 当前" : "")").tag(Optional(face.id))
                            }
                        }.disabled(guestOperations.isBusy)
                            .accessibilityIdentifier("simulator.guest.watchface-picker")
                        Button { Task { await guestOperations.loadInventory() } } label: {
                            Image(systemName: "arrow.clockwise")
                        }.disabled(!guestOperations.canRefresh).help("刷新设备表盘")
                            .accessibilityLabel("刷新设备表盘")
                    }
                    action("使用表盘", icon: "checkmark.circle", enabled: guestOperations.canSelect) {
                        Task { await guestOperations.selectWatchface() }
                    }.accessibilityIdentifier("simulator.guest.select")
                    applicationInventory
                }
                if let progress = guestOperations.progressLabel {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text(progress)
                    }.font(.system(size: 12)).accessibilityIdentifier("simulator.guest.progress")
                } else if let result = guestOperations.resultMessage {
                    Text(result).font(.system(size: 12)).accessibilityIdentifier("simulator.guest.result")
                }
                if let error = guestOperations.errorMessage {
                    Text(error).font(.system(size: 12)).foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                        .accessibilityIdentifier("simulator.guest.error")
                }
                DisclosureGroup("安装包检查与暂存") {
                    SimulatorActionFlow {
                        action("检查安装包", icon: "doc.text.magnifyingglass", enabled: controlsAvailable && session.isAttached) { choose(.inspectPackage) }
                        action("暂存安装包", icon: "tray.and.arrow.down", enabled: controlsAvailable && session.isAttached) { choose(.stagePackage) }
                    }
                    Text("仅处理宿主文件，不会安装到设备。")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                }
                if !session.packageReport.isEmpty {
                    DisclosureGroup("检查与暂存结果", isExpanded: $showingPackageReport) {
                        Text(session.packageReport)
                            .font(.system(size: 12, design: .monospaced))
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            if presentation == .full, showingLogs {
                SimulatorSectionDivider()
                logPanel.frame(height: 340)
            }
        }
        .simulatorSurface()
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }

    private var applicationInventory: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("应用").font(.subheadline.weight(.medium))
                Spacer()
                Button { Task { await guestOperations.loadInventory(kind: .quickapp) } } label: {
                    Image(systemName: "arrow.clockwise")
                }.disabled(!guestOperations.canRefresh).help("刷新设备应用")
                    .accessibilityLabel("刷新设备应用")
                    .accessibilityIdentifier("simulator.guest.apps-refresh")
            }
            if guestOperations.applications.isEmpty {
                Text(guestOperations.applicationsLoaded ? "暂无应用" : "正在读取…")
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
            } else {
                ForEach(guestOperations.applications) { app in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(app.name.isEmpty ? app.id : app.name).font(.system(size: 13, weight: .medium))
                        Text("\(app.id) · 版本 \(app.version)")
                            .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                    }.fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                }
                Text("从设备应用列表打开。")
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
            }
        }.accessibilityIdentifier("simulator.guest.apps-inventory")
    }

    private var logPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(instanceName.map { "\($0) · 日志" } ?? "日志", systemImage: "text.alignleft")
                .font(.headline).accessibilityAddTraits(.isHeader)
            SimulatorActionFlow {
                Picker("日志来源", selection: $logName) {
                    Text("UART 0").tag("uart0.log")
                    Text("UART 1").tag("uart1.log")
                    Text("UART 2").tag("uart2.log")
                    Text("QEMU").tag("qemu.log")
                    Text("标准错误").tag("stderr.log")
                }.fixedSize().disabled(!session.isAttached)
                Toggle("跟随最新日志", isOn: $followLogBottom).toggleStyle(.checkbox)
                action("重连日志", icon: "arrow.clockwise", enabled: isActive && session.isAttached) {
                    if let id = session.manifest?.id, let directory = runtime.sessionURL {
                        logStream.start(directory: directory, sessionID: id, name: logName)
                    }
                }
            }
            if session.isAttached && logStream.detail != "实时刷新 · 最多保留 256 KiB" {
                Text(logStream.detail)
                    .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
            }
            SimulatorLogTextView(text: logStream.text, followBottom: followLogBottom,
                                 discardedUTF16: logStream.discardedUTF16, epoch: logStream.epoch)
                .frame(minHeight: 70, maxHeight: .infinity)
                .accessibilityIdentifier("simulator.live-log")
        }.padding(16).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(SimulatorTheme.surface)
    }

    private var firmwareLibrarySection: some View {
        SimulatorSection(title: "固件", icon: "applewatch") {
            if runtime.selectableFirmwareItems.isEmpty {
                Text(runtime.hasLoadedCatalog ? "尚未导入固件；点击「导入固件…」选择完整 OTA 包" : "正在读取固件库")
                    .fontWeight(.medium)
            } else {
                Picker("已导入固件", selection: Binding(get: { pendingProfileID ?? runtime.selectedProfileID }, set: { id in
                    if runtime.isServing {
                        pendingProfileID = id == runtime.selectedProfileID ? nil : id
                    } else if let id {
                        Task { await runtime.switchFirmware(to: id) }
                    } else { runtime.selectedProfileID = nil }
                })) {
                    Text("选择启动固件…").tag(Optional<String>.none)
                    ForEach(runtime.selectableFirmwareItems) { item in
                        Text("\(item.displayName)\(item.source == .imported ? " · 已导入" : "")")
                            .tag(Optional(item.id))
                    }
                }
                // Switching is free whenever this app owns or has stopped the
                // core; only an externally attached session cannot be switched.
                .disabled(!controlsAvailable || (session.isAttached && !runtime.isServing))
                .help(session.isAttached && !runtime.isServing ? "已连接外部会话；断开后可切换固件"
                    : "可切换本型号的任意已导入固件；切换固件会保留数据盘")
                .accessibilityIdentifier("simulator.firmware-picker")
                if let pending = pendingProfileID, runtime.isServing {
                    SimulatorActionFlow {
                        action("切换并重新开机", icon: "arrow.triangle.2.circlepath", enabled: controlsAvailable) {
                            pendingProfileID = nil
                            Task { await recording.stop(); await runtime.switchFirmware(to: pending) }
                        }
                        .accessibilityIdentifier("simulator.switch-firmware")
                        action("取消", icon: "xmark", enabled: true) { pendingProfileID = nil }
                    }
                    Text("将停止设备、保存数据盘，然后用所选固件冷启动；切换固件会保留数据盘。")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else if runtime.selectableFirmwareItems.count > 1 {
                    Text("可随时切换本型号的其他已导入固件；切换固件会保留数据盘。")
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if let item = runtime.selectedFirmware {
                Text("\(item.model) · \(item.width) × \(item.height)")
                    .foregroundStyle(SimulatorTheme.secondary)
                if !item.availability.ready {
                    Text(item.availability.reason.isEmpty ? "此固件尚缺少完整运行资源" : item.availability.reason)
                        .foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if runtime.importCandidateIDs.count > 1 && runtime.selectedProfileID == nil {
                Text("导入文件对应多个型号，请选择匹配的设备后运行。")
                    .foregroundStyle(SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            SimulatorActionFlow {
                action("运行", icon: "play.fill", enabled: controlsAvailable && runtime.canRunSelectedFirmware) {
                    Task {
                        guard controlsAvailable, await runtime.prepareSelectedFirmware(), controlsAvailable else { return }
                        if !runtime.preparedNeedsReview { await runtime.startApproved() }
                        else { review = SimulatorRunReview(summary: runtime.selectedSummary, runLimitSeconds: runtime.preparedRunLimitSeconds) }
                    }
                }
                .keyboardShortcut("r", modifiers: .command)
                .help(runtime.runUnavailableReason ?? (sharing.isSharing || sharing.sessionURL != nil
                    ? "请先停止 Studio 连接共享" : "按本实例的硬件预设开机（⌘R）"))
                .accessibilityIdentifier("simulator.run-firmware")
                action("导入固件…", icon: "square.and.arrow.down", enabled: controlsAvailable && runtime.libraryAvailable && !runtime.isServing && !session.isAttached) { choose(.importFirmware) }
                    .help(runtime.isServing || session.isAttached ? "停止设备后可导入固件"
                        : runtime.libraryAvailable ? "选择完整 OTA 包导入到固件库" : "固件库暂不可用；请先点击「刷新目录」")
                    .accessibilityIdentifier("simulator.import-firmware")
                action("刷新目录", icon: "arrow.clockwise", enabled: controlsAvailable && !runtime.isServing && !session.isAttached) {
                    Task { await runtime.loadCatalog() }
                }
            }
            if let failure = runtime.importFailureMessage {
                Label("上次导入失败：\(failure)", systemImage: "exclamationmark.circle")
                    .foregroundStyle(SimulatorTheme.text)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("simulator.import-error")
            }
            if let diagnostic = runtime.importDiagnostic {
                Text(diagnostic.summary).fontWeight(.medium)
                Text(diagnostic.details).font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("simulator.firmware-diagnostic")
            }
            if let diagnosticError = runtime.importDiagnosticError {
                Text(diagnosticError).foregroundStyle(SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if runtime.modelConstraint != nil {
                DisclosureGroup("存储状态") {
                    Text(runtime.instanceStorageMessage)
                        .font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if !runtime.libraryStatus.isEmpty && runtime.libraryStatus != "固件库已就绪" {
                Text(runtime.libraryStatus).font(.system(size: 12))
                    .foregroundStyle(SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if runtime.isBusy { ProgressView().controlSize(.small).accessibilityLabel("正在检查固件") }
        }
    }

    private func devicePane(_ geometry: SimulatorDisplayGeometry) -> some View {
        SimulatorSection(title: "设备画面", icon: "applewatch") {
            SimulatorNativeCanvas(image: session.image, geometry: geometry, acceptsInput: session.canInput, onInput: session.sendInput)
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
            frame: session.frame, isAttached: session.isAttached, logName: logsVisible ? logName : nil,
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
        case .importFirmware: Task { guard controlsAvailable else { return }; await runtime.importFirmware(url) }
        case .qemu: runtime.qemuURL = url
        case .firmware: runtime.firmwareURL = url
        case .resource: runtime.resourceURL = url
        case .session: Task { guard controlsAvailable else { return }; await runtime.attachExisting(url) }
        case .inspectPackage: showingPackageReport = true; session.inspectPackage(url)
        case .stagePackage: showingPackageReport = true; session.stagePackage(url)
        case .installGuestPackage:
            guard guestOperations.canInstall, installSelectionHost == session.manifest?.id,
                  installSelectionPeer == guestOperations.identity else {
                uiError = "设备连接已变化，请重新选择安装包。"; return
            }
            Task { await guestOperations.install(url) }
        }
    }
    private var errorMessage: String? { uiError ?? sharing.errorMessage ?? runtime.errorMessage ?? session.errorMessage }
    private func dismissErrors() { uiError = nil; sharing.errorMessage = nil; runtime.errorMessage = nil; session.errorMessage = nil }
}

private enum SimulatorFileChoice { case importFirmware, qemu, firmware, resource, session, inspectPackage, stagePackage, installGuestPackage }
private struct SimulatorRunReview: Identifiable {
    let id = UUID()
    let summary: String
    let runLimitSeconds: Int
}

private struct SimulatorRunConfirmation: View {
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
