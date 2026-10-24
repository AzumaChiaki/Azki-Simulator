import AppKit
import SwiftUI

@MainActor struct SimulatorFirmwareControls: View {
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    let recording: SimulatorRecordingModel
    let instanceName: String?
    @State private var pendingProfileID: String?

    private var session: SimulatorSessionModel { runtime.runtimeSession }
    private var controlsAvailable: Bool { SimulatorWorkspaceView.coreControlsAvailable(runtime: runtime, sharing: sharing) }

    var body: some View {
        Section("固件") {
            firmwareSelection
            selectedFirmwareDetails
            if runtime.modelConstraint != nil {
                DisclosureGroup("存储状态") {
                    Text(runtime.instanceStorageMessage)
                }
            }
            if !runtime.libraryStatus.isEmpty && runtime.libraryStatus != "固件库已就绪" {
                Text(runtime.libraryStatus)
            }
            if runtime.isBusy { ProgressView("正在检查固件") }
            importDiagnostics
        }
        .onChange(of: runtime.isServing) { _, serving in if !serving { pendingProfileID = nil } }
        .onChange(of: runtime.selectedProfileID) { _, _ in pendingProfileID = nil }
        .onChange(of: runtime.resetEpoch) { _, _ in pendingProfileID = nil }
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
            Text(runtime.hasLoadedCatalog ? "尚未导入固件；使用「导入固件…」选择完整 OTA 包" : "正在读取固件库")
        } else {
            Picker("已导入固件", selection: firmwareSelectionBinding) {
                Text("选择启动固件…").tag(Optional<String>.none)
                ForEach(runtime.selectableFirmwareItems) { item in
                    Text(firmwareLabel(item)).tag(Optional<String>.some(item.id))
                }
            }
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
            Button("切换并重新开机", systemImage: "arrow.triangle.2.circlepath") {
                pendingProfileID = nil
                Task { await recording.stop(); await runtime.switchFirmware(to: pending) }
            }
            .disabled(!controlsAvailable)
            .accessibilityIdentifier("simulator.switch-firmware")
            Button("取消") { pendingProfileID = nil }
            Text("将停止设备、保存数据盘，然后用所选固件冷启动。")
        } else if runtime.selectableFirmwareItems.count > 1 {
            Text("可随时切换本型号的其他已导入固件；切换固件会保留数据盘。")
        }
    }

    @ViewBuilder private var selectedFirmwareDetails: some View {
        if let item = runtime.selectedFirmware {
            LabeledContent("型号", value: "\(item.model) · \(item.width) × \(item.height)")
            if !item.availability.ready {
                Text(item.availability.reason.isEmpty ? "此固件尚缺少完整运行资源" : item.availability.reason)
            }
        }
        if runtime.importCandidateIDs.count > 1 && runtime.selectedProfileID == nil {
            Text("导入文件对应多个型号，请选择匹配的设备后运行。")
        }
    }

    @ViewBuilder private var importDiagnostics: some View {
        if let failure = runtime.importFailureMessage {
            Label("上次导入失败：\(failure)", systemImage: "exclamationmark.circle")
                .accessibilityIdentifier("simulator.import-error")
        }
        if let diagnostic = runtime.importDiagnostic {
            Text(diagnostic.summary)
            Text(diagnostic.details).font(.body.monospaced()).textSelection(.enabled)
                .accessibilityIdentifier("simulator.firmware-diagnostic")
        }
        if let diagnosticError = runtime.importDiagnosticError {
            Text(diagnosticError)
        }
    }
}
