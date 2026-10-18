import AppKit
import SwiftUI

/// Both windows use the same runtime, recording and guest operation objects.
/// This view only owns presentation state; the workspace remains the single
/// owner of guest polling, session invalidation and core lifetime.
@MainActor struct SimulatorDeviceControls<WindowActions: View>: View {
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    @Bindable var recording: SimulatorRecordingModel
    @Bindable var guestOperations: SimulatorGuestOperationsModel
    let instanceName: String?
    @ViewBuilder let windowActions: () -> WindowActions
    @State private var connectionTimeoutText = ""
    @State private var screenshotURL: URL?
    @State private var screenshotError: String?
    @State private var showingPackageReport = true
    @State private var choosingFile = false
    @State private var fileChoice: PackageChoice = .install
    @State private var installSelectionHost: UUID?
    @State private var installSelectionPeer: SimulatorGuestOperationsModel.Identity?
    @State private var uiError: String?

    private enum PackageChoice { case inspect, stage, install }
    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var controlsAvailable: Bool { SimulatorWorkspaceView.coreControlsAvailable(runtime: runtime, sharing: sharing) }
    /// Stop stays available while starting or checking: it cancels that work
    /// instead of making the user wait for a stuck start to time out.
    private var canStop: Bool {
        !runtime.isStopping && (runtime.isServing || runtime.isBusy || (session.isRunning && !session.isCommandInFlight))
    }
    private var sessionRunLimit: Int? {
        guard let value = session.status?["maxRuntimeSeconds"], case .number(let seconds) = value,
              seconds.isFinite, seconds.rounded() == seconds, (1...Double(StandaloneRunLimits.maximumSeconds)).contains(seconds) else { return nil }
        return Int(seconds)
    }
    private var watchButtonsAvailable: Bool {
        guard controlsAvailable, !runtime.isStopping, session.isRunning,
              case .array(let capabilities)? = session.status?["capabilities"] else { return false }
        return capabilities.contains(.string("device-button"))
    }

    var body: some View {
        VStack(spacing: 0) {
            SimulatorFirmwareControls(runtime: runtime, sharing: sharing, recording: recording, instanceName: instanceName)
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
                    if ["miwear.watch.p65", "miwear.watch.q65"].contains(runtime.modelConstraint ?? runtime.selectedFirmware?.model) {
                        action("点击表冠", icon: "button.programmable", enabled: watchButtonsAvailable) {
                            session.perform("device-button", arguments: ["button": .string("crown"), "clicks": .number(1)])
                        }
                        .accessibilityIdentifier("simulator.watch.crown")
                        action("单击下键", icon: "hand.tap", enabled: watchButtonsAvailable) {
                            session.perform("device-button", arguments: ["button": .string("lower"), "clicks": .number(1)])
                        }
                        .accessibilityIdentifier("simulator.watch.lower.single")
                        action("双击下键", icon: "hand.tap", enabled: watchButtonsAvailable) {
                            session.perform("device-button", arguments: ["button": .string("lower"), "clicks": .number(2)])
                        }
                        .help("依次执行两次单击下键")
                        .accessibilityIdentifier("simulator.watch.lower.double")
                    }
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
                    windowActions()
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
                        choose(.install)
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
                        action("检查安装包", icon: "doc.text.magnifyingglass", enabled: controlsAvailable && session.isAttached) { choose(.inspect) }
                        action("暂存安装包", icon: "tray.and.arrow.down", enabled: controlsAvailable && session.isAttached) { choose(.stage) }
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
        }
        .fileImporter(isPresented: $choosingFile, allowedContentTypes: [.item]) { result in
            switch result {
            case .success(let url): selected(url)
            case .failure(let error):
                if (error as NSError).code != NSUserCancelledError { uiError = error.localizedDescription }
            }
        }
        .onAppear { connectionTimeoutText = String(runtime.connectionSettings.timeoutSeconds) }
        .onChange(of: runtime.connectionSettings.timeoutSeconds) { _, seconds in
            connectionTimeoutText = String(seconds)
        }
        .onChange(of: runtime.resetEpoch) { _, _ in
            uiError = nil; screenshotURL = nil; screenshotError = nil
            showingPackageReport = true
        }
        .alert(instanceName.map { "Azki Simulator · \($0)" } ?? "Azki Simulator",
               isPresented: Binding(get: { uiError != nil }, set: { if !$0 { uiError = nil } })) {
            Button("确定") { uiError = nil }
        } message: { Text(uiError ?? "") }
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

    private func action(_ title: String, icon: String, enabled: Bool, run: @escaping () -> Void) -> some View {
        Button(action: run) { Label(title, systemImage: icon).simulatorActionLabel() }
            .disabled(!enabled)
    }

    private func choose(_ choice: PackageChoice) { fileChoice = choice; choosingFile = true }
    private func selected(_ url: URL) {
        guard controlsAvailable else { uiError = "请先停止只读共享或等待当前操作结束。"; return }
        switch fileChoice {
        case .inspect: showingPackageReport = true; session.inspectPackage(url)
        case .stage: showingPackageReport = true; session.stagePackage(url)
        case .install:
            guard guestOperations.canInstall, installSelectionHost == session.manifest?.id,
                  installSelectionPeer == guestOperations.identity else {
                uiError = "设备连接已变化，请重新选择安装包。"; return
            }
            Task { await guestOperations.install(url) }
        }
    }
}
