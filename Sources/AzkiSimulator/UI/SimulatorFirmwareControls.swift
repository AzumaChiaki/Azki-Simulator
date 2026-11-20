import AppKit
import SwiftUI

@MainActor struct SimulatorFirmwareControls: View {
    @Bindable var runtime: StandaloneRuntimeModel
    @Bindable var sharing: HostStatusSharingModel
    let recording: SimulatorRecordingModel
    let instanceName: String?
    var onImportFirmware: (() -> Void)? = nil
    @State private var pendingProfileID: String?

    private var session: SimulatorSessionModel { runtime.runtimeSession }

    var body: some View {
        Section("固件") {
            firmwareSelection
            selectedFirmwareDetails
            if let onImportFirmware {
                HStack(spacing: 8) {
                    Button {
                        onImportFirmware()
                    } label: {
                        Label("导入固件…", systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
                    .disabled(runtime.isStopping)
                    Spacer(minLength: 0)
                }
            }
            if runtime.modelConstraint != nil {
                DisclosureGroup("存储状态") {
                    Text(runtime.instanceStorageMessage).font(.caption).foregroundStyle(.secondary)
                }
            }
            if !runtime.libraryStatus.isEmpty && runtime.libraryStatus != "固件库已就绪" {
                Text(runtime.libraryStatus).font(.caption).foregroundStyle(.secondary)
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
            Text(runtime.hasLoadedCatalog ? "尚未导入固件；点击「导入固件…」选择固件包即可" : "正在读取固件库")
                .font(.caption).foregroundStyle(.secondary)
        } else {
            Picker("已导入固件", selection: firmwareSelectionBinding) {
                Text("选择启动固件…").tag(Optional<String>.none)
                ForEach(runtime.selectableFirmwareItems) { item in
                    Text(firmwareLabel(item)).tag(Optional<String>.some(item.id))
                }
            }
            .disabled(runtime.isStopping)
            .help("可随时切换本型号的任意已导入固件；切换固件会保留数据盘")
            .accessibilityIdentifier("simulator.firmware-picker")
            pendingFirmwareNotice
        }
    }

    private func firmwareLabel(_ item: BundledFirmwareItem) -> String {
        item.displayName + (item.source == .imported ? " · 已导入" : "")
    }

    @ViewBuilder private var pendingFirmwareNotice: some View {
        if let pending = pendingProfileID, runtime.isServing {
            HStack(spacing: 8) {
                Button {
                    pendingProfileID = nil
                    Task {
                        await recording.stop()
                        if sharing.isSharing || sharing.sessionURL != nil { _ = await sharing.disable() }
                        await runtime.switchFirmware(to: pending)
                    }
                } label: {
                    Label("切换并重新开机", systemImage: "arrow.triangle.2.circlepath")
                }
                .buttonStyle(GeminiNeonButtonStyle(tone: .primary, compact: true))
                .disabled(runtime.isStopping)
                .accessibilityIdentifier("simulator.switch-firmware")

                Button("取消") { pendingProfileID = nil }
                    .buttonStyle(GeminiNeonButtonStyle(tone: .subtle, compact: true))
            }
            Text("将停止设备、保存数据盘，然后用所选固件冷启动。")
                .font(.caption).foregroundStyle(.secondary)
        } else if runtime.selectableFirmwareItems.count > 1 {
            Text("可随时切换本型号的其他已导入固件；切换固件会保留数据盘。")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var selectedFirmwareDetails: some View {
        if let item = runtime.selectedFirmware {
            LabeledContent("型号", value: item.width > 0
                ? "\(item.model) · \(item.width) × \(item.height)" : item.model)
            if let kind = item.packageKind {
                LabeledContent("固件类型", value: kind == "incremental" ? "增量包" : kind == "full" ? "完整包" : "固件资料")
            }
            if let base = item.baseVersion { LabeledContent("基础版本", value: base) }
            if !item.availability.ready {
                Text(item.availability.reason.isEmpty ? "此固件尚缺少完整运行资源" : item.availability.reason)
                    .font(.caption).foregroundStyle(GeminiPalette.amber)
            }
        }
        if runtime.importCandidateIDs.count > 1 && runtime.selectedProfileID == nil {
            Text("导入文件对应多个型号，请选择匹配的设备后运行。")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var importDiagnostics: some View {
        if let failure = runtime.importFailureMessage {
            Label("上次导入失败：\(failure)", systemImage: "exclamationmark.circle")
                .foregroundStyle(GeminiPalette.amber)
                .accessibilityIdentifier("simulator.import-error")
        }
        if let diagnostic = runtime.importDiagnostic {
            Text(diagnostic.summary).font(.caption)
            Text(diagnostic.details).font(.body.monospaced()).textSelection(.enabled)
                .accessibilityIdentifier("simulator.firmware-diagnostic")
        }
        if let diagnosticError = runtime.importDiagnosticError {
            Text(diagnosticError).font(.caption).foregroundStyle(.red)
        }
    }
}
