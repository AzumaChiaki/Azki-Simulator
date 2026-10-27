import AppKit
import SwiftUI

@MainActor struct SimulatorStorageWindow: View {
    let library: SimulatorInstanceLibrary
    @State private var model = SimulatorStorageModel()

    var body: some View {
        SimulatorStorageManagementView(library: library, model: model)
    }
}

/// Visible accounting and safe reclamation for the app's own library.
/// The view never inspects an image itself; all sizes and actions come from the
/// bundled, deny-default helper that already owns the library.
@MainActor struct SimulatorStorageManagementView: View {
    let library: SimulatorInstanceLibrary
    @Bindable var model: SimulatorStorageModel
    @State private var keep = 4
    @State private var confirmingDeep = false
    @State private var pendingOptimize: PendingOptimize?
    @State private var pendingDelete: PendingDelete?

    private struct PendingDelete: Identifiable {
        let id: String
        let name: String
        let registered: UUID?
    }

    private struct PendingOptimize: Identifiable {
        let id = UUID()
        let name: String
        let keep: Int
        let scoped: String?
    }

    private struct SessionRow: Identifiable {
        let id: String
        let instanceName: String
        let session: SimulatorStorageReport.Session
    }

    private var report: SimulatorStorageReport? { model.report }

    private var sessions: [SessionRow] {
        guard let report else { return [] }
        return report.instances.flatMap { entry in
            entry.sessions.map { SessionRow(id: $0.path, instanceName: name(for: entry), session: $0) }
        }.sorted { $0.session.bytes > $1.session.bytes }
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    header
                    if let report {
                        summary(report)
                        categories(report)
                        instanceSection(report)
                        sessionSection
                        footnote
                    } else if model.isBusy {
                        ContentUnavailableView("正在读取存储占用", systemImage: "externaldrive",
                                               description: Text("统计实例快照、固件库和临时会话。"))
                            .frame(maxWidth: .infinity, minHeight: 260)
                    } else {
                        ContentUnavailableView("无法读取存储占用", systemImage: "externaldrive.badge.exclamationmark",
                                               description: Text(model.errorMessage ?? "请稍后重试。"))
                            .frame(maxWidth: .infinity, minHeight: 260)
                    }
                }
                .padding(20)
            }
            Divider()
            actionBar
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
        }
        .frame(minWidth: 620, minHeight: 440)
        .task { await model.refresh() }
        .confirmationDialog("深度清理存储", isPresented: $confirmingDeep, titleVisibility: .visible) {
            Button("保留 2 份快照并清理", role: .destructive) {
                confirmingDeep = false
                Task { await model.maintain(keep: 2) }
            }
            Button("取消", role: .cancel) { confirmingDeep = false }
        } message: {
            Text("将每个实例的已提交快照裁剪到最近 2 份，并去重字节完全相同的镜像。更早的恢复点会永久删除；运行中的实例会被跳过。")
        }
        .confirmationDialog("整理存储", isPresented: Binding(
            get: { pendingOptimize != nil }, set: { if !$0 { pendingOptimize = nil } }),
            titleVisibility: .visible, presenting: pendingOptimize) { item in
            Button("保留 \(item.keep) 份并整理", role: .destructive) {
                let keep = item.keep, scoped = item.scoped
                pendingOptimize = nil
                Task { await model.maintain(keep: keep, instanceID: scoped) }
            }
            Button("取消", role: .cancel) { pendingOptimize = nil }
        } message: { item in
            Text("将把\(item.name)的已提交快照裁剪到最近 \(item.keep) 份，并把字节完全相同的镜像改为 APFS 克隆。更早的恢复点会永久删除；运行中的实例会被跳过。")
        }
        .confirmationDialog("删除实例", isPresented: Binding(
            get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
            titleVisibility: .visible, presenting: pendingDelete) { item in
            Button("删除「\(item.name)」", role: .destructive) {
                pendingDelete = nil
                Task {
                    if let registered = item.registered {
                        _ = await library.delete(registered)
                    } else {
                        _ = await model.deleteOrphan(item.id)
                    }
                    await model.refresh()
                }
            }
            Button("取消", role: .cancel) { pendingDelete = nil }
        } message: { item in
            Text(item.registered == nil
                 ? "将移除未登记实例「\(item.name)」的整个数据目录和仍属于它的已停止临时会话。此操作不可撤销。"
                 : "将停止并移除「\(item.name)」，同时删除它的所有 NAND/NOR 快照、固件设置和仍属于它的已停止临时会话。已导入的固件包保留。")
        }
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text("存储管理").font(.title2.bold())
                if let report {
                    Text(report.root)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                    Text(report.freeBytes > 0
                         ? "卷可用 \(bytes(report.freeBytes)) / \(bytes(report.volumeBytes))"
                         : "卷可用空间暂不可读")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text("统计 Azki Simulator 自己的实例、快照、固件库和临时会话。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 8)
            Button {
                Task { await model.refresh() }
            } label: {
                Label("刷新", systemImage: "arrow.clockwise")
            }
            .disabled(model.isBusy)
            .accessibilityIdentifier("storage.refresh")
            Button {
                NSWorkspace.shared.open(URL(fileURLWithPath: report?.root ?? SimulatorDataLocations.appSupportRoot.path))
            } label: {
                Label("在访达中打开", systemImage: "folder")
            }
            .disabled(report == nil)
            .accessibilityIdentifier("storage.reveal-root")
        }
    }

    private func summary(_ report: SimulatorStorageReport) -> some View {
        HStack(spacing: 10) {
            StorageCard(title: "应用数据总计", value: bytes(report.totalBytes),
                        detail: "库 \(bytes(report.libraryBytes ?? report.totalBytes))", systemImage: "internaldrive")
            StorageCard(title: "可优化（估算）", value: bytes(report.reclaimableBytes),
                        detail: "旧快照、重复镜像与已停止会话", systemImage: "arrow.down.circle")
            StorageCard(title: "实例快照", value: bytes(report.instances.reduce(0) { $0 + $1.revisionBytes }),
                        detail: "\(report.instances.count) 个实例", systemImage: "clock.arrow.circlepath")
            StorageCard(title: "临时会话", value: bytes(report.sessionsBytes),
                        detail: "\(report.instances.reduce(0) { $0 + $1.sessions.count }) 个", systemImage: "hourglass")
        }
    }

    private func categories(_ report: SimulatorStorageReport) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("空间分布").font(.headline)
            ForEach(report.categories) { category in
                HStack(spacing: 10) {
                    Text(category.name).frame(width: 110, alignment: .leading)
                    ProgressView(value: Double(category.bytes),
                                 total: Double(max(report.totalBytes, 1)))
                        .tint(category.id == "instances" ? Color.accentColor : Color.secondary)
                    Text(bytes(category.bytes))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 82, alignment: .trailing)
                }
                .accessibilityIdentifier("storage.category.\(category.id)")
            }
        }
    }

    private func instanceSection(_ report: SimulatorStorageReport) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("实例").font(.headline)
                Spacer()
                Text("按实例整理时只影响该实例，运行中的实例会自动跳过")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if report.instances.isEmpty {
                Text("还没有实例数据。创建并启动实例后，快照会显示在这里。")
                    .font(.callout).foregroundStyle(.secondary)
                    .padding(.vertical, 10)
            } else {
                VStack(spacing: 0) {
                    ForEach(report.instances) { entry in
                        instanceRow(entry)
                        if entry.id != report.instances.last?.id { Divider() }
                    }
                }
                .background(Color(nsColor: .quaternaryLabelColor).opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
            }
        }
    }

    private func instanceRow(_ entry: SimulatorStorageReport.Instance) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "applewatch")
                .font(.title3)
                .foregroundStyle(.secondary)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(name(for: entry)).font(.headline).lineLimit(1)
                    if entry.busy {
                        Text("运行中").font(.caption2.bold())
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(.green.opacity(0.16), in: Capsule())
                    } else if entry.pending == true {
                        Text("待整理").font(.caption2.bold())
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(.orange.opacity(0.18), in: Capsule())
                    }
                    if entry.unsafe {
                        Text("需检查").font(.caption2.bold())
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(.orange.opacity(0.18), in: Capsule())
                    }
                }
                Text("\(entry.model ?? "未知机型") · \(entry.revisionCount) 个快照 · 实例总计 \(bytes(entry.bytes))")
                    .font(.caption).foregroundStyle(.secondary)
                HStack(spacing: 10) {
                    Text("快照数据 \(bytes(entry.revisionBytes))")
                    if entry.duplicateBytes > 0 {
                        Text("可去重 \(bytes(entry.duplicateBytes))").foregroundStyle(.orange)
                    }
                    if entry.erasedBytes > 0 {
                        Text("空白镜像 \(bytes(entry.erasedBytes))").foregroundStyle(.secondary)
                    }
                }
                .font(.caption2)
            }
            Spacer(minLength: 8)
            Button("整理") {
                pendingOptimize = PendingOptimize(name: "「\(name(for: entry))」", keep: keep, scoped: entry.id)
            }
            .disabled(model.isBusy)
            .help("按当前保留数整理这个实例的快照")
            .accessibilityIdentifier("storage.maintain.\(entry.id)")
            Button {
                let base = URL(fileURLWithPath: report?.root ?? "")
                let path = base.appendingPathComponent("instances/\(entry.id)/storage")
                NSWorkspace.shared.activateFileViewerSelecting([path])
            } label: {
                Image(systemName: "folder")
            }
            .buttonStyle(.borderless)
            .help("在访达中显示这个实例的数据")
            Button(role: .destructive) {
                pendingDelete = PendingDelete(id: entry.id, name: name(for: entry), registered: registeredID(entry))
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .help("删除实例及其全部快照")
            .accessibilityIdentifier("storage.delete.\(entry.id)")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
    }

    @ViewBuilder private var sessionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("临时会话").font(.headline)
                Spacer()
                Text("只在确认进程已停止后清理")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if sessions.isEmpty {
                Text("没有残留的临时会话。")
                    .font(.callout).foregroundStyle(.secondary)
            } else {
                ForEach(sessions.prefix(8)) { row in
                    HStack(spacing: 8) {
                        Image(systemName: row.session.state == "running" ? "play.circle" : "hourglass")
                            .foregroundStyle(row.session.state == "running" ? Color.green : Color.secondary)
                        Text(row.instanceName).lineLimit(1)
                        Text(row.session.state == "running" ? "运行中" : row.session.state == "unknown" ? "待确认" : "已停止")
                            .font(.caption).foregroundStyle(.secondary)
                        Spacer()
                        Text(bytes(row.session.bytes)).font(.caption.monospacedDigit())
                    }
                    .font(.caption)
                }
                if sessions.count > 8 {
                    Text("另有 \(sessions.count - 8) 个会话").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var footnote: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("从本版本起，快照和运行副本在 APFS 上使用写时复制：未修改的数据块只占一份空间；完全未写入的 NAND 共享同一个空白模板。")
            Text("“可优化”是逻辑估算，包含可裁剪的旧快照、字节完全相同的镜像和已停止会话；实际释放量以卷可用空间为准。")
        }
        .font(.caption2)
        .foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
    }

    private var actionBar: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Text("保留快照").font(.caption).foregroundStyle(.secondary)
                Picker("保留快照", selection: $keep) {
                    Text("2 份").tag(2)
                    Text("4 份").tag(4)
                    Text("8 份").tag(8)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 190)
                Button("优化存储", systemImage: "wand.and.stars") {
                    pendingOptimize = PendingOptimize(name: "整个库", keep: keep, scoped: nil)
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.isBusy)
                .accessibilityIdentifier("storage.optimize")
                Button("深度清理…", systemImage: "trash.slash", role: .destructive) {
                    confirmingDeep = true
                }
                .disabled(model.isBusy || report == nil)
                Button("清理临时会话") {
                    Task { await model.cleanupSessions() }
                }
                .disabled(model.isBusy)
                Spacer(minLength: 0)
                if model.isBusy { ProgressView().controlSize(.small) }
            }
            HStack(spacing: 6) {
                if let message = model.lastMessage {
                    Label(message, systemImage: "checkmark.circle").font(.caption).foregroundStyle(.green)
                }
                if let error = model.errorMessage {
                    Label(error, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange)
                        .lineLimit(2)
                }
            }
            .frame(minHeight: 16)
        }
    }

    private func name(for entry: SimulatorStorageReport.Instance) -> String {
        if let match = library.instances.first(where: { $0.id.uuidString.lowercased() == entry.id }) {
            return match.record.name
        }
        return "未登记实例 \(entry.id.prefix(8))"
    }

    private func registeredID(_ entry: SimulatorStorageReport.Instance) -> UUID? {
        library.instances.first(where: { $0.id.uuidString.lowercased() == entry.id })?.id
    }

    private func bytes(_ value: Int64) -> String {
        SimulatorStorageModel.bytes(value)
    }
}

private struct StorageCard: View {
    let title: String
    let value: String
    let detail: String
    let systemImage: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Label(title, systemImage: systemImage)
                .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            Text(value).font(.title3.bold()).monospacedDigit().lineLimit(1)
            Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(Color(nsColor: .quaternaryLabelColor).opacity(0.22), in: RoundedRectangle(cornerRadius: 9))
    }
}
