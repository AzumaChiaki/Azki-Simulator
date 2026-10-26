import AppKit
import SwiftUI

@MainActor struct SimulatorDeviceControls: View {
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    @Bindable var recording: SimulatorRecordingModel
    @Bindable var guestOperations: SimulatorGuestOperationsModel
    let instanceName: String?
    @State private var showingPackageReport = true
    @State private var choosingFile = false
    @State private var fileChoice: PackageChoice = .install
    @State private var installSelectionHost: UUID?
    @State private var installSelectionPeer: SimulatorGuestOperationsModel.Identity?
    @State private var uninstallCandidate: SimulatorGuestOperationsModel.QuickApp?
    @State private var uninstallSelectionHost: UUID?
    @State private var uninstallSelectionPeer: SimulatorGuestOperationsModel.Identity?
    @State private var uiError: String?

    private enum PackageChoice { case inspect, stage, install }
    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var controlsAvailable: Bool { SimulatorWorkspaceView.coreControlsAvailable(runtime: runtime, sharing: sharing) }

    var body: some View {
        Group {
            Button("连接虚拟手环", systemImage: "link") { Task { await guestOperations.connect() } }
                .disabled(!guestOperations.canConnect)
                .help("连接此实例的本机虚拟通信通道")
                .accessibilityIdentifier("simulator.guest.connect")
            Button("安装…", systemImage: "square.and.arrow.down") {
                installSelectionHost = session.manifest?.id
                installSelectionPeer = guestOperations.identity
                choose(.install)
            }
            .disabled(!guestOperations.canInstall)
            .accessibilityIdentifier("simulator.install")
            Text(session.isRunning ? guestOperations.connectionLabel : "运行设备后可连接")
                .accessibilityIdentifier("simulator.guest.connection-state")
            if guestOperations.isAuthenticated {
                Picker("表盘", selection: $guestOperations.selectedWatchfaceID) {
                    Text(guestOperations.inventoryLoaded ? "选择表盘" : "正在读取…").tag(Optional<String>.none)
                    ForEach(guestOperations.inventory) { face in
                        Text("\(face.name)\(face.isCurrent ? " · 当前" : "")").tag(Optional(face.id))
                    }
                }.disabled(guestOperations.isBusy)
                    .accessibilityIdentifier("simulator.guest.watchface-picker")
                Button("刷新表盘", systemImage: "arrow.clockwise") { Task { await guestOperations.loadInventory() } }
                    .disabled(!guestOperations.canRefresh)
                    .accessibilityLabel("刷新设备表盘")
                Button("使用表盘", systemImage: "checkmark.circle") { Task { await guestOperations.selectWatchface() } }
                    .disabled(!guestOperations.canSelect)
                    .accessibilityIdentifier("simulator.guest.select")
                applicationInventory
            }
            if let progress = guestOperations.progressLabel {
                LabeledContent {
                    ProgressView()
                } label: {
                    Text(progress)
                }.accessibilityIdentifier("simulator.guest.progress")
            } else if let result = guestOperations.resultMessage {
                Text(result).accessibilityIdentifier("simulator.guest.result")
            }
            if let error = guestOperations.errorMessage {
                Text(error).foregroundStyle(.red).textSelection(.enabled)
                    .accessibilityIdentifier("simulator.guest.error")
            }
            DisclosureGroup("安装包检查与暂存") {
                Button("检查安装包") { choose(.inspect) }
                    .disabled(!(controlsAvailable && session.isAttached))
                Button("暂存安装包") { choose(.stage) }
                    .disabled(!(controlsAvailable && session.isAttached))
                Text("仅处理宿主文件，不会安装到设备。")
            }
            if !session.packageReport.isEmpty {
                DisclosureGroup("检查与暂存结果", isExpanded: $showingPackageReport) {
                    Text(session.packageReport).font(.body.monospaced()).textSelection(.enabled)
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
        .onChange(of: runtime.resetEpoch) { _, _ in
            uiError = nil; showingPackageReport = true
        }
        .alert(instanceName ?? "Azki Simulator", isPresented: Binding(get: { uiError != nil }, set: { if !$0 { uiError = nil } })) {
            Button("好") { uiError = nil }
        } message: { Text(uiError ?? "") }
        .alert("卸载应用？", isPresented: Binding(get: { uninstallCandidate != nil }, set: { if !$0 { uninstallCandidate = nil } })) {
            Button("取消", role: .cancel) { uninstallCandidate = nil }
            Button("卸载", role: .destructive) { confirmUninstall() }
        } message: {
            if let app = uninstallCandidate { Text("将从虚拟手环卸载“\(app.name.isEmpty ? app.id : app.name)”。") }
        }
    }

    private var applicationInventory: some View {
        Group {
            HStack {
                Text("应用")
                Spacer()
                Button("刷新应用", systemImage: "arrow.clockwise") { Task { await guestOperations.loadInventory(kind: .quickapp) } }
                    .disabled(!guestOperations.canRefresh)
                    .accessibilityLabel("刷新设备应用")
                    .accessibilityIdentifier("simulator.guest.apps-refresh")
            }
            if guestOperations.applications.isEmpty {
                Text(guestOperations.applicationsLoaded ? "暂无应用" : "正在读取…")
            } else {
                ForEach(guestOperations.applications) { app in
                    HStack {
                        LabeledContent(app.name.isEmpty ? app.id : app.name, value: "\(app.id) · 版本 \(app.version)")
                        Button("删除", systemImage: "trash", role: .destructive) {
                            uninstallCandidate = app
                            uninstallSelectionHost = session.manifest?.id
                            uninstallSelectionPeer = guestOperations.identity
                        }
                        .disabled(!guestOperations.canUninstall(app))
                        .help("卸载应用")
                        .accessibilityIdentifier("simulator.guest.app-uninstall.\(app.id)")
                    }
                }
                Text("从设备应用列表打开。")
            }
        }.accessibilityIdentifier("simulator.guest.apps-inventory")
    }

    private func confirmUninstall() {
        guard let app = uninstallCandidate else { return }
        uninstallCandidate = nil
        guard guestOperations.canUninstall(app), uninstallSelectionHost == session.manifest?.id,
              uninstallSelectionPeer == guestOperations.identity else {
            uiError = "设备连接已变化或操作正忙，请重新选择应用。"
            return
        }
        Task { await guestOperations.uninstall(app) }
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
