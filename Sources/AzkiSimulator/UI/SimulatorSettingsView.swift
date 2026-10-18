import AppKit
import SwiftUI

/// App Settings: manage imported firmware images and open the storage folder.
@MainActor struct SimulatorSettingsView: View {
    let library: SimulatorInstanceLibrary
    @State private var deleting: BundledFirmwareItem?
    @State private var confirmingDeleteAll = false

    private var items: [BundledFirmwareItem] { library.catalog.firmwareItems }

    var body: some View {
        TabView {
            imageManagement
                .tabItem { Label("镜像管理", systemImage: "externaldrive.fill.badge.xmark") }
            storage
                .tabItem { Label("存储目录", systemImage: "folder") }
        }
        .frame(width: 580, height: 440)
    }

    private var imageManagement: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("镜像管理").font(.title2)
            Text("这里列出已导入并保存到本机的固件镜像（完整 OTA）。删除后仅移除该镜像本身，不影响已创建的实例记录。")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if library.catalog.libraryAvailable || library.catalog.hasLoadedCatalog {
                if items.isEmpty {
                    ContentUnavailableView("还没有已保存的镜像", systemImage: "externaldrive")
                } else {
                    List {
                        ForEach(items) { item in
                            HStack(alignment: .center, spacing: 10) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(item.displayName).lineLimit(1)
                                    Text("\(item.model) · \(item.firmwareVersion) · \(item.otaSHA256.prefix(12))")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer(minLength: 0)
                                Button(role: .destructive) { deleting = item } label: {
                                    Image(systemName: "trash")
                                }
                                .buttonStyle(.borderless)
                                .help("删除该已保存的镜像")
                                .accessibilityIdentifier("settings.delete-firmware.\(item.otaSHA256)")
                            }
                            .padding(.vertical, 4)
                        }
                    }
                    .listStyle(.inset)
                    // Attached to the List, not the whole sheet: SwiftUI presents
                    // only one confirmationDialog per view, so stacking both
                    // dialogs here silently disabled the single-item delete.
                    .confirmationDialog("删除已保存的镜像", isPresented: Binding(get: { deleting != nil },
                        set: { if !$0 { deleting = nil } }), titleVisibility: .visible, presenting: deleting) { item in
                        Button("删除「\(item.displayName)」", role: .destructive) {
                            let sha = item.otaSHA256
                            deleting = nil
                            Task { await library.catalog.deleteFirmware(sha256: sha) }
                        }
                        Button("取消", role: .cancel) { deleting = nil }
                    } message: { item in
                        Text("将从固件库中彻底移除该 OTA 及其独占的对象文件，并回收对应磁盘空间。正在使用该固件的实例请先停止。")
                    }
                }
            } else {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("正在读取固件库…").font(.caption).foregroundStyle(.secondary)
                }
            }
            if let message = library.catalog.errorMessage {
                Text(message).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            }
            Divider()
            HStack {
                Text("共 \(items.count) 个已保存镜像")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer(minLength: 0)
                Button {
                    Task { await library.catalog.loadCatalog() }
                } label: {
                    Label("刷新", systemImage: "arrow.clockwise")
                }
                .disabled(library.catalog.isBusy)
                .help("重新读取固件库列表")
                .accessibilityIdentifier("settings.refresh-firmware")
                Button(role: .destructive) {
                    guard !items.isEmpty else { return }
                    confirmingDeleteAll = true
                } label: {
                    Label("删除全部镜像", systemImage: "trash.fill")
                }
                .disabled(items.isEmpty)
                .accessibilityIdentifier("settings.delete-all-firmware")
                .confirmationDialog("删除全部镜像", isPresented: $confirmingDeleteAll, titleVisibility: .visible) {
                    Button("删除全部 \(items.count) 个镜像", role: .destructive) {
                        confirmingDeleteAll = false
                        Task { await library.catalog.deleteAllFirmware() }
                    }
                    Button("取消", role: .cancel) { confirmingDeleteAll = false }
                } message: {
                    Text("将清空整个固件库：移除所有已导入的 OTA 及其对象文件，并回收全部对应磁盘空间。此操作不可撤销，已创建的实例记录保留。")
                }
            }
            Spacer(minLength: 0)
        }
        .padding(20)
        .task {
            if !library.catalog.hasLoadedCatalog { await library.catalog.loadCatalog() }
        }
    }

    private var storage: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("存储目录").font(.title2)
            Text("应用的所有数据都保存在以下目录：实例记录、已导入的固件库、运行快照与临时会话。")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 8) {
                Text(SimulatorDataLocations.appSupportRoot.path)
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                Spacer(minLength: 0)
            }
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .textBackgroundColor)))
            Button("在访达中打开") {
                NSWorkspace.shared.open(SimulatorDataLocations.appSupportRoot)
            }
            .accessibilityIdentifier("settings.open-storage")
            Spacer(minLength: 0)
        }
        .padding(20)
    }
}
