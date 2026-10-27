import AppKit
import SwiftUI

@MainActor struct SimulatorSettingsView: View {
    let library: SimulatorInstanceLibrary
    @State private var deleting: BundledFirmwareItem?
    @State private var confirmingDeleteAll = false
    @State private var storageModel = SimulatorStorageModel()

    private var items: [BundledFirmwareItem] { library.catalog.firmwareItems }

    var body: some View {
        TabView {
            Tab("镜像", systemImage: "externaldrive") { imageManagement }
            Tab("存储", systemImage: "folder") {
                SimulatorStorageManagementView(library: library, model: storageModel)
            }
        }
        .frame(minWidth: 720, minHeight: 480)
    }

    private var imageManagement: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("这里列出已导入并保存到本机的固件镜像（完整 OTA）。删除后仅移除该镜像本身，不影响已创建的实例记录。")
                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
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
                        }
                    }
                    .listStyle(.inset)
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
                    Text("正在读取固件库…").foregroundStyle(.secondary)
                }
            }
            if let message = library.catalog.errorMessage {
                Text(message).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Text("共 \(items.count) 个已保存镜像")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer(minLength: 0)
                Button("刷新", systemImage: "arrow.clockwise") {
                    Task { await library.catalog.loadCatalog() }
                }
                .disabled(library.catalog.isBusy)
                .help("重新读取固件库列表")
                .accessibilityIdentifier("settings.refresh-firmware")
                Button("删除全部镜像", systemImage: "trash", role: .destructive) {
                    guard !items.isEmpty else { return }
                    confirmingDeleteAll = true
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

}
