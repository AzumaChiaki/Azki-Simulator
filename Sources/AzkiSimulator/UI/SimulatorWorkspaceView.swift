import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor struct SimulatorWorkspaceView {
    static func canEnableSharing(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel) -> Bool {
        !runtime.isStopping && !sharing.isSharing && !sharing.isBusy
    }

    static func coreControlsAvailable(runtime: StandaloneRuntimeModel, sharing: HostStatusSharingModel) -> Bool {
        !runtime.isStopping
    }

    static func canStopSharing(_ sharing: HostStatusSharingModel) -> Bool {
        (sharing.isSharing || sharing.sessionURL != nil) && !sharing.isBusy
    }
}

@MainActor struct SimulatorInstanceInspector: View {
    let library: SimulatorInstanceLibrary
    @Bindable var instance: SimulatorInstanceContext
    @Binding var logsPresented: Bool
    @Binding var importFirmware: Bool
    @State private var choosingFile = false
    @State private var fileChoice: SimulatorFileChoice = .session
    @State private var showingPreparedFiles = false
    @State private var showingAdvanced = false
    @State private var showingSharing = false
    @State private var uiError: String?
    @State private var debugText = ""
    @State private var debugDocument = SimulatorDebugDocument(data: Data())
    @State private var exportingDebug = false
    @State private var adcDraft = PMUADCDraft()
    @State private var editingFirmwareSettings = false
    @State private var confirmingReset = false

    private var runtime: StandaloneRuntimeModel { instance.runtime }
    private var sharing: HostStatusSharingModel { instance.sharing }
    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var recording: SimulatorRecordingModel { instance.recording }
    private var guestOperations: SimulatorGuestOperationsModel { instance.guestOperations }
    private var controlsAvailable: Bool { SimulatorInstanceActions.controlsAvailable(instance) }
    private var allFilesSelected: Bool { runtime.qemuURL != nil && runtime.firmwareURL != nil && runtime.resourceURL != nil }
    private var adcAvailable: Bool {
        guard session.isRunning, case .array(let inputs)? = session.status?["hardwareInputs"] else { return false }
        return inputs.contains(.string("pmu.adcRaw"))
    }
    private var parsedADC: [Int]? { adcDraft.values }
    private var adcProfileID: String? {
        guard case .string(let profile)? = session.status?["profileID"] else { return nil }
        return profile
    }

    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        Form {
            SimulatorFirmwareControls(
                runtime: runtime,
                sharing: sharing,
                recording: recording,
                instanceName: instance.record.name,
                onImportFirmware: { importFirmware = true }
            )
            sessionSection
            Section("设备数据") {
                SimulatorDeviceDataView(runtime: runtime, session: session, enabled: controlsAvailable) {
                    editingFirmwareSettings = true
                }
                HStack(spacing: 8) {
                    Button {
                        Task {
                            await recording.stop()
                            if sharing.isSharing || sharing.sessionURL != nil { _ = await sharing.disable() }
                            await runtime.coldRestart()
                        }
                    } label: {
                        Label(runtime.isServing ? "冷重启" : "冷启动", systemImage: "arrow.clockwise.circle")
                    }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                    .disabled(runtime.isStopping)
                    .accessibilityIdentifier("simulator.cold-restart")
                    Spacer(minLength: 0)
                }
            }
            DisclosureGroup("应用与表盘") { packageSection }
            DisclosureGroup("连接") {
                SimulatorDeviceConnectionView(library: library, instance: instance, bluetooth: instance.bluetooth)
            }
            DisclosureGroup("调试", isExpanded: $showingAdvanced) { debugSection }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .background(GeminiPalette.inspectorBackground(for: colorScheme))
        .controlSize(.small)
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
        .sheet(isPresented: $editingFirmwareSettings) {
            SimulatorFirmwareSettingsSheet(runtime: runtime, restartAvailable: true) {
                Task {
                    await recording.stop()
                    if sharing.isSharing || sharing.sessionURL != nil { _ = await sharing.disable() }
                    await runtime.coldRestart()
                }
            }
        }
        .onChange(of: runtime.resetEpoch) { _, _ in
            debugText = ""; uiError = nil; adcDraft = PMUADCDraft(); confirmingReset = false
        }
        .alert("检查器", isPresented: Binding(get: { uiError != nil }, set: { if !$0 { uiError = nil } })) {
            Button("好") { uiError = nil }
        } message: { Text(uiError ?? "") }
        .confirmationDialog("重置实例", isPresented: $confirmingReset, titleVisibility: .visible) {
            Button("重置「\(instance.record.name)」") {
                Task {
                    await recording.stop()
                    _ = await sharing.disable()
                    await runtime.resetInstance()
                }
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("将停止设备并清除运行缓存与错误。已保存的数据、固件设置和已导入固件都会保留。")
        }
    }

    @ViewBuilder private var sessionSection: some View {
        Section("会话") {
            Toggle("按真实屏幕形状裁切", isOn: Binding(
                get: { DisplaySettings.shared.clipToScreenShape },
                set: { DisplaySettings.shared.clipToScreenShape = $0 }
            ))
            .accessibilityIdentifier("simulator.clip-screen-shape")
            Text("开启后，实时画布、截图（透明圆角 PNG）与录屏均按设备真实屏幕形状裁切；关闭可查看完整矩形帧。")
                .font(.caption).foregroundStyle(.secondary)
            TextField("连接超时（秒）", value: Binding(
                get: { runtime.connectionSettings.timeoutSeconds },
                set: { if ConnectionSettings.timeoutRange.contains($0) { runtime.connectionSettings.timeoutSeconds = $0 } }
            ), format: .number.grouping(.never))
            .accessibilityIdentifier("simulator.connection-timeout")
            Text("1–300，用于虚拟设备连接；下次操作生效。")
                .font(.caption).foregroundStyle(.secondary)
            if runtime.isStopping {
                ProgressView("正在停止设备…")
            } else if !session.isRunning && !runtime.statusMessage.isEmpty {
                Text(runtime.statusMessage).font(.caption).foregroundStyle(.secondary)
            }
            if let issue = session.transientIssue {
                Label(issue, systemImage: "exclamationmark.triangle").foregroundStyle(GeminiPalette.amber)
                    .accessibilityIdentifier("simulator.transient-issue")
            }
            HStack(spacing: 8) {
                Button {
                    logsPresented.toggle()
                } label: {
                    Label(logsPresented ? "隐藏日志" : "显示日志",
                          systemImage: logsPresented ? "rectangle.bottomthird.inset.filled" : "text.alignleft")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: logsPresented ? .active : .secondary, compact: true))
                .help(logsPresented ? "隐藏实时日志" : "显示实时日志")
                .accessibilityIdentifier("simulator.logs-session")

                Button {
                    confirmingReset = true
                } label: {
                    Label("重置实例…", systemImage: "arrow.counterclockwise.circle")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                .help("停止设备并清除此实例的运行缓存与错误；已保存的数据保留")
                .disabled(runtime.isStopping)
                .accessibilityIdentifier("simulator.reset-instance")
            }
            if session.isAttached && session.image == nil {
                GeminiStatusPill(text: "等待画面", tone: .connecting, compact: true)
            }
            if recording.isRecording {
                GeminiStatusPill(
                    text: "录制中 \(Int(recording.elapsedTime) / 60):\(String(format: "%02d", Int(recording.elapsedTime) % 60))",
                    tone: .recording,
                    compact: true
                )
            } else if recording.isFinishing {
                ProgressView("正在保存录屏…")
            } else if let url = recording.savedFileURL {
                Button("在桌面显示录屏") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                    .help(url.lastPathComponent)
            }
            if let message = recording.errorMessage {
                Text(message).font(.caption).foregroundStyle(.red)
                if let url = recording.recoveryFileURL {
                    Button("显示可恢复视频") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                }
            }
        }
    }

    @ViewBuilder private var packageSection: some View {
        SimulatorDeviceControls(runtime: runtime, sharing: sharing, recording: recording,
                                guestOperations: guestOperations, instanceName: instance.record.name)
    }

    @ViewBuilder private var debugSection: some View {
        DisclosureGroup("Studio 连接", isExpanded: $showingSharing) {
            HStack(spacing: 8) {
                Button {
                    Task {
                        if runtime.isServing || session.isAttached {
                            library.showToast("正在停止设备以开启 Studio 连接…")
                            await recording.stop()
                            _ = await runtime.stop()
                            session.detach()
                        }
                        if sharing.sessionURL != nil && !sharing.isSharing {
                            _ = await sharing.disable()
                        }
                        await sharing.enable()
                    }
                } label: {
                    Label("启用 Studio 连接", systemImage: "link")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: sharing.isSharing ? .active : .secondary, compact: true))
                .disabled(!SimulatorWorkspaceView.canEnableSharing(runtime: runtime, sharing: sharing))
                .accessibilityIdentifier("simulator.enable-status-sharing")

                Button {
                    Task { await sharing.disable() }
                } label: {
                    Label("停止共享", systemImage: "eject")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                .disabled(!SimulatorWorkspaceView.canStopSharing(sharing))
                .accessibilityIdentifier("simulator.disable-status-sharing")
            }
            Text(sharing.isSharing ? "Studio 可扫描连接并推送表盘" : sharing.sessionURL != nil ? "目录清理待重试" : "启用后，Studio 会自动扫描。开启时若设备正在运行会自动停止。")
                .font(.caption).foregroundStyle(.secondary)
            if let url = sharing.sessionURL {
                Text(url.path).font(.body.monospaced()).textSelection(.enabled)
                    .accessibilityIdentifier("simulator.status-sharing-directory")
            }
            Text("支持状态、日志与表盘文件接收；尚不支持客体安装运行。")
                .font(.caption).foregroundStyle(.secondary)
            if sharing.isBusy { ProgressView("正在更新 Studio 连接") }
        }
        HStack(spacing: 8) {
            Button {
                captureDebug(export: false)
            } label: {
                Label("采集调试信息", systemImage: "waveform.path.ecg")
            }
            .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
            .accessibilityIdentifier("simulator.capture-debug")

            Button {
                captureDebug(export: true)
            } label: {
                Label("导出调试快照…", systemImage: "square.and.arrow.up")
            }
            .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
        }
        if !debugText.isEmpty {
            Text(debugText).font(.caption.monospaced()).textSelection(.enabled)
        }
        DisclosureGroup("会话与运行文件") {
            if let manifest = session.manifest {
                Text("会话 \(manifest.id.uuidString.lowercased())").font(.body.monospaced()).textSelection(.enabled)
            }
            Button {
                choose(.session)
            } label: {
                Label("连接已有会话", systemImage: "folder")
            }
            .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
            .disabled(runtime.isStopping)
            .accessibilityIdentifier("simulator.connect")
            fileRow("QEMU 核心", url: runtime.qemuURL, choice: .qemu)
            fileRow("系统固件", url: runtime.firmwareURL, choice: .firmware)
            fileRow("资源固件", url: runtime.resourceURL, choice: .resource)
            HStack(spacing: 8) {
                Button {
                    Task {
                        if sharing.isSharing || sharing.sessionURL != nil { _ = await sharing.disable() }
                        if runtime.isServing || session.isAttached { _ = await runtime.stop(); session.detach() }
                        await runtime.prepare()
                    }
                } label: {
                    Label("准备会话", systemImage: "checkmark.shield")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                .disabled(!allFilesSelected || runtime.isStopping)
                .accessibilityIdentifier("simulator.prepare")

                Button {
                    instance.runReview = SimulatorRunReview(summary: runtime.selectedSummary, runLimitSeconds: runtime.preparedRunLimitSeconds)
                } label: {
                    Label("核对并启动", systemImage: "play.fill")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
                .disabled(!runtime.canStartPrepared)
                .accessibilityIdentifier("simulator.review-start")
            }
            if runtime.isBusy { ProgressView("正在处理会话") }
            if runtime.sessionURL != nil {
                DisclosureGroup("来源与 SHA-256", isExpanded: $showingPreparedFiles) {
                    Text(runtime.selectedSummary).font(.body.monospaced()).textSelection(.enabled)
                }
            }
        }
        Section("PMU 原始采样") {
            Text("按原始计数改写 PMU 的八路采样。日常电量、充电、温度请用「设备数据」。")
                .font(.caption).foregroundStyle(.secondary)
            if adcAvailable {
                ForEach(PMUADCDescriptors.channels, id: \.self) { index in
                    TextField(PMUADCDescriptors.title(index, profileID: adcProfileID), text: $adcDraft.text[index])
                        .accessibilityLabel("\(PMUADCDescriptors.title(index, profileID: adcProfileID))，未校准原始值")
                        .accessibilityIdentifier("simulator.adc.\(index)")
                    Text(PMUADCDescriptors.purpose(index, profileID: adcProfileID)).font(.caption).foregroundStyle(.secondary)
                    Text(PMUADCDescriptors.readCount(index, response: session.adcState)).font(.caption.monospaced())
                    Text(PMUADCDescriptors.readStatus(index, response: session.adcState)).font(.caption).foregroundStyle(.secondary)
                }
                HStack(spacing: 6) {
                    Button("读取当前采样") { session.perform("pmu-adc-get") }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                        .accessibilityIdentifier("simulator.adc.read")
                    Button("应用原始输入") {
                        guard let values = parsedADC else { return }
                        session.perform("pmu-adc-set", arguments: ["values": .array(values.map { .number(Double($0)) })])
                    }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
                    .disabled(parsedADC == nil)
                    .accessibilityIdentifier("simulator.adc.apply")
                }
                HStack(spacing: 6) {
                    Button("恢复启动默认值") { adcDraft.useBootDefaults() }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .subtle, compact: true))
                        .disabled(adcDraft.observed == nil)
                    Button("撤销修改") { adcDraft.discardEdits() }
                        .buttonStyle(GeminiNeonButtonStyle(tone: .subtle, compact: true))
                        .disabled(!adcDraft.hasChanges)
                }
                if adcDraft.hasChanges {
                    Text(parsedADC == nil ? "每项需要输入 0…65535 之间的整数。" : "有尚未应用的修改。")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if let state = session.adcState {
                    DisclosureGroup("采样详情") {
                        Text(state.prettyDescription).font(.body.monospaced()).textSelection(.enabled)
                    }
                }
            } else {
                Text(session.isRunning ? "当前核心不支持 ADC 输入" : "运行后可编辑支持的原始输入")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .onChange(of: session.adcState, initial: true) { _, response in adcDraft.receive(response) }
    }

    private func fileRow(_ title: String, url: URL?, choice: SimulatorFileChoice) -> some View {
        LabeledContent(title) {
            HStack {
                Text(url?.path ?? "尚未选择").foregroundStyle(.secondary).lineLimit(1)
                Button("选择…") { choose(choice) }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
            }
        }
    }

    private func captureDebug(export: Bool) {
        let snapshot = SimulatorDebugSnapshot(manifest: session.manifest, status: session.status,
            frame: session.frame, isAttached: session.isAttached, logName: instance.logStream.selectedName,
            logText: instance.logStream.text)
        let frame = snapshot.frame.map { "\($0.width) × \($0.height) · 序号 \($0.sequence)" } ?? "尚无完整帧"
        debugText = "固件：\(snapshot.profileID ?? "未报告")\n状态：\(snapshot.reportedState ?? "未报告")\n画面：\(frame)\n核心进程：\(snapshot.hostProcessRunning.map { $0 ? "运行" : "停止" } ?? "未报告")\n客体安装：\(snapshot.guestInstallSupported.map { $0 ? "后端声明支持" : "未实现" } ?? "未报告")\n采集时间：\(snapshot.capturedAt.formatted())"
        if export {
            do { debugDocument = SimulatorDebugDocument(data: try snapshot.encodedJSON()); exportingDebug = true }
            catch { uiError = error.localizedDescription }
        }
    }

    private func choose(_ choice: SimulatorFileChoice) { fileChoice = choice; choosingFile = true }
    private func selected(_ url: URL, for choice: SimulatorFileChoice) {
        switch choice {
        case .qemu: runtime.qemuURL = url
        case .firmware: runtime.firmwareURL = url
        case .resource: runtime.resourceURL = url
        case .session:
            Task {
                if sharing.isSharing || sharing.sessionURL != nil { _ = await sharing.disable() }
                await runtime.attachExisting(url)
            }
        }
    }
}

private enum SimulatorFileChoice { case qemu, firmware, resource, session }

struct SimulatorRunConfirmation: View {
    let summary: String
    let runLimitSeconds: Int
    let cancel: () -> Void
    let confirm: () -> Void

    var body: some View {
        NavigationStack {
            Form {
                Text("将运行以下核心与固件，\(runLimitSeconds == 0 ? "直到手动停止" : "本次最长 " + StandaloneRunLimits.describe(runLimitSeconds))。请核对文件来源与指纹。")
                Section("指纹") {
                    Text(summary).font(.body.monospaced()).textSelection(.enabled)
                }
            }
            .formStyle(.grouped)
            .navigationTitle("确认运行所选核心与固件")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消", action: cancel).keyboardShortcut(.cancelAction)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(runLimitSeconds == 0 ? "确认运行" : "确认运行（最长 \(StandaloneRunLimits.describe(runLimitSeconds))）", action: confirm)
                        .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(minWidth: 560, minHeight: 360)
    }
}
