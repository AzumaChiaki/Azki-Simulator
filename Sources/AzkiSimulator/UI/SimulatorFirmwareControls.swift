import AppKit
import SwiftUI

/// The full firmware workflow is shared by the workspace and device window.
/// Every action remains bound to the originating instance's runtime.
@MainActor struct SimulatorFirmwareControls: View {
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    let recording: SimulatorRecordingModel
    let instanceName: String?
    @State private var pendingProfileID: String?
    @State private var choosingFile = false
    @State private var review: SimulatorRunReview?
    @State private var uiError: String?

    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var controlsAvailable: Bool { SimulatorWorkspaceView.coreControlsAvailable(runtime: runtime, sharing: sharing) }

    var body: some View {
        SimulatorSection(title: "固件", icon: "applewatch") {
            firmwareSelection
            selectedFirmwareDetails
            firmwareActions
            importDiagnostics
            libraryStatus
        }
        .fileImporter(isPresented: $choosingFile, allowedContentTypes: [.item], onCompletion: importSelectedFirmware)
        .sheet(item: $review) { prepared in
            SimulatorRunConfirmation(summary: prepared.summary, runLimitSeconds: prepared.runLimitSeconds) {
                review = nil
            } confirm: { confirmRun(prepared) }
        }
        .onChange(of: runtime.isServing) { _, serving in if !serving { pendingProfileID = nil } }
        .onChange(of: runtime.selectedProfileID) { _, _ in pendingProfileID = nil }
        .onChange(of: runtime.resetEpoch) { _, _ in
            pendingProfileID = nil; review = nil; uiError = nil
        }
        .alert(instanceName.map { "Azki Simulator · \($0)" } ?? "Azki Simulator",
               isPresented: Binding(get: { uiError != nil }, set: { if !$0 { uiError = nil } })) {
            Button("确定") { uiError = nil }
        } message: { Text(uiError ?? "") }
    }

    private var firmwareSelectionBinding: Binding<String?> {
        Binding(get: { pendingProfileID ?? runtime.selectedProfileID }, set: { id in
            if runtime.isServing {
                pendingProfileID = id == runtime.selectedProfileID ? nil : id
            } else if let id {
                Task { await runtime.switchFirmware(to: id) }
            } else { runtime.selectedProfileID = nil }
        })
    }

    @ViewBuilder private var firmwareSelection: some View {
        if runtime.selectableFirmwareItems.isEmpty {
            Text(runtime.hasLoadedCatalog ? "尚未导入固件；点击「导入固件…」选择完整 OTA 包" : "正在读取固件库")
                .fontWeight(.medium)
        } else {
            Picker("已导入固件", selection: firmwareSelectionBinding) {
                Text("选择启动固件…").tag(Optional<String>.none)
                ForEach(runtime.selectableFirmwareItems) { item in
                    Text(firmwareLabel(item)).tag(Optional<String>.some(item.id))
                }
            }
            // Only an externally attached session cannot be switched.
            .disabled(!controlsAvailable || (session.isAttached && !runtime.isServing))
            .help(session.isAttached && !runtime.isServing ? "已连接外部会话；断开后可切换固件"
                : "可切换本型号的任意已导入固件；切换固件会保留数据盘")
            .accessibilityIdentifier("simulator.firmware-picker")
            pendingFirmwareNotice
        }
    }

    private func firmwareLabel(_ item: BundledFirmwareItem) -> String {
        item.displayName + (item.source == .imported ? " · 已导入" : "")
    }

    @ViewBuilder private var pendingFirmwareNotice: some View {
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

    @ViewBuilder private var selectedFirmwareDetails: some View {
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
    }

    private var firmwareActions: some View {
        SimulatorActionFlow {
            action("运行", icon: "play.fill", enabled: controlsAvailable && runtime.canRunSelectedFirmware) {
                prepareAndRun()
            }
            .keyboardShortcut("r", modifiers: .command)
            .help(runtime.runUnavailableReason ?? (sharing.isSharing || sharing.sessionURL != nil
                ? "请先停止 Studio 连接共享" : "按本实例的硬件预设开机（⌘R）"))
            .accessibilityIdentifier("simulator.run-firmware")
            action("导入固件…", icon: "square.and.arrow.down", enabled: controlsAvailable && runtime.libraryAvailable && !runtime.isServing && !session.isAttached) { choosingFile = true }
                .help(runtime.isServing || session.isAttached ? "停止设备后可导入固件"
                    : runtime.libraryAvailable ? "选择完整 OTA 包导入到固件库" : "固件库暂不可用；请先点击「刷新目录」")
                .accessibilityIdentifier("simulator.import-firmware")
            action("刷新目录", icon: "arrow.clockwise", enabled: controlsAvailable && !runtime.isServing && !session.isAttached) {
                Task { await runtime.loadCatalog() }
            }
        }
    }

    @ViewBuilder private var importDiagnostics: some View {
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
    }

    @ViewBuilder private var libraryStatus: some View {
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

    private func importSelectedFirmware(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            guard controlsAvailable, !runtime.isServing, !session.isAttached else {
                uiError = "设备状态已变化，请先停止设备再导入固件。"; return
            }
            Task { await runtime.importFirmware(url) }
        case .failure(let error):
            if (error as NSError).code != NSUserCancelledError { uiError = error.localizedDescription }
        }
    }

    private func prepareAndRun() {
        Task {
            guard controlsAvailable, await runtime.prepareSelectedFirmware(), controlsAvailable else { return }
            if !runtime.preparedNeedsReview { await runtime.startApproved() }
            else { review = SimulatorRunReview(summary: runtime.selectedSummary, runLimitSeconds: runtime.preparedRunLimitSeconds) }
        }
    }

    private func confirmRun(_ prepared: SimulatorRunReview) {
        review = nil
        guard controlsAvailable, runtime.canStartPrepared, runtime.selectedSummary == prepared.summary,
              runtime.preparedRunLimitSeconds == prepared.runLimitSeconds else {
            uiError = "准备的文件已变化，请重新准备并核对指纹。"; return
        }
        Task { await runtime.startApproved() }
    }

    private func action(_ title: String, icon: String, enabled: Bool, run: @escaping () -> Void) -> some View {
        Button(action: run) { Label(title, systemImage: icon).simulatorActionLabel() }
            .disabled(!enabled)
    }
}
