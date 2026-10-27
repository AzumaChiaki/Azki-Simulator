import Foundation
import Observation

/// One read-only snapshot of the app's own storage roots. The helper never
/// returns file contents, only bounded sizes and revision metadata.
nonisolated struct SimulatorStorageReport: Decodable, Sendable {
    struct Category: Decodable, Identifiable, Sendable {
        let id: String
        let name: String
        let bytes: Int64
    }

    struct Session: Decodable, Identifiable, Sendable {
        var id: String { path }
        let path: String
        let bytes: Int64
        let files: Int?
        let state: String
        let unsafe: Bool?
    }

    struct Instance: Decodable, Identifiable, Sendable {
        let id: String
        let model: String?
        let bytes: Int64
        let revisionBytes: Int64
        let revisionCount: Int
        let committedCount: Int?
        let head: String?
        let prunable2Bytes: Int64
        let prunable4Bytes: Int64
        let duplicateBytes: Int64
        let erasedBytes: Int64
        let sessionsBytes: Int64
        let sessions: [Session]
        let busy: Bool
        let pending: Bool?
        let unsafe: Bool
        let lastUsed: Double?
    }

    struct Retention: Decodable, Sendable {
        let suggested: Int
        let maximum: Int
        let minimum: Int
    }

    let version: Int
    let root: String
    let totalBytes: Int64
    let libraryBytes: Int64?
    let sessionsBytes: Int64
    let freeBytes: Int64
    let volumeBytes: Int64
    let unsafe: Bool?
    let categories: [Category]
    let instances: [Instance]
    let reclaimableBytes: Int64
    let prunableBytes: Int64
    let duplicateBytes: Int64
    let stoppedSessionBytes: Int64
    let erasedBytes: Int64
    let retention: Retention
}

nonisolated struct SimulatorStorageMaintenance: Decodable, Sendable {
    struct Revision: Decodable, Sendable {
        let instanceID: String?
        let revision: String?
    }

    struct Issue: Decodable, Sendable {
        let instanceID: String?
        let revision: String?
        let image: String?
        let reason: String?
        let error: String?
    }

    let dryRun: Bool?
    let keep: Int?
    let removedRevisions: [Revision]?
    let prunedBytes: Int64?
    let clonedFiles: Int?
    let duplicateFiles: Int?
    let duplicateBytes: Int64?
    let skipped: [Issue]?
    let errors: [Issue]?
    let sessionsRemoved: [String]?
    let reclaimedBytes: Int64?
    let freedBytes: Int64?
}

/// Runs the bundled storage commands through the same deny-default helper as
/// firmware imports. It never opens an instance's NAND image itself.
@MainActor @Observable
final class SimulatorStorageModel {
    private let service: BundledFirmwareService
    private(set) var report: SimulatorStorageReport?
    private(set) var isBusy = false
    private(set) var lastMessage: String?
    var errorMessage: String?

    init(service: BundledFirmwareService = BundledFirmwareService()) {
        self.service = service
    }

    func refresh() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }
        await loadReport()
    }

    private func loadReport() async {
        do {
            report = try decode(SimulatorStorageReport.self, from: await service.run(["storage-report"]))
            errorMessage = nil
        } catch {
            errorMessage = "读取存储占用失败：\(error.localizedDescription)"
        }
    }

    /// `instanceID` scopes the pass to one instance; nil means the whole library.
    func maintain(keep: Int, instanceID: String? = nil) async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }
        var arguments = ["storage-maintain", "--keep", String(max(1, min(keep, 32)))]
        if let instanceID { arguments += ["--instance-id", instanceID] }
        do {
            let result = try decode(SimulatorStorageMaintenance.self, from: await service.run(arguments))
            let freed = result.freedBytes ?? 0
            let removed = result.removedRevisions?.count ?? 0
            let cloned = result.clonedFiles ?? 0
            let sessions = result.sessionsRemoved?.count ?? 0
            if freed == 0 && cloned > 0 && removed == 0 && sessions == 0 {
                lastMessage = "已整理 \(cloned) 个重复镜像；这些副本可能已共享数据块，未新增可释放空间。"
            } else {
                lastMessage = "已回收 \(Self.bytes(freed))：整理 \(cloned) 个重复镜像，删除 \(removed) 个旧快照" +
                    (sessions > 0 ? "，清理 \(sessions) 个临时会话。" : "。")
            }
            if let issues = result.errors, !issues.isEmpty {
                errorMessage = "部分项目未整理：" + (issues.first?.error ?? issues.first?.reason ?? "未知原因")
            } else {
                errorMessage = nil
            }
            await loadReport()
        } catch {
            errorMessage = "整理存储失败：\(error.localizedDescription)"
        }
    }

    func cleanupSessions() async {
        guard !isBusy else { return }
        isBusy = true
        defer { isBusy = false }
        do {
            struct Receipt: Decodable { let removed: [String]?; let kept: [String]? }
            let receipt = try decode(Receipt.self, from: await service.run(["sessions-cleanup"]))
            lastMessage = "已清理 \(receipt.removed?.count ?? 0) 个已停止的临时会话" +
                ((receipt.kept?.count ?? 0) > 0 ? "，保留 \(receipt.kept?.count ?? 0) 个运行中或未确认的会话。" : "。")
            errorMessage = nil
            await loadReport()
        } catch {
            errorMessage = "清理临时会话失败：\(error.localizedDescription)"
        }
    }

    /// Delete data for a folder whose record is no longer in instances.json.
    /// Registered instances go through SimulatorInstanceLibrary so their record
    /// is removed first; this is only for a leftover folder.
    func deleteOrphan(_ identifier: String) async -> Bool {
        guard !isBusy else { return false }
        isBusy = true
        defer { isBusy = false }
        do {
            struct Receipt: Decodable { let removed: Bool?; let sessionsRemoved: [String]? }
            let receipt = try decode(Receipt.self, from: await service.run(
                ["instance-delete", "--instance-id", identifier]))
            lastMessage = "已删除未登记实例的数据" +
                ((receipt.sessionsRemoved?.isEmpty == false) ? "及其临时会话。" : "。")
            errorMessage = nil
            await loadReport()
            return true
        } catch {
            errorMessage = "删除实例数据失败：\(error.localizedDescription)"
            return false
        }
    }

    private func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        do { return try JSONDecoder().decode(type, from: data) }
        catch { throw SimulatorStorageError.invalidResponse(error.localizedDescription) }
    }

    static func bytes(_ value: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: max(0, value), countStyle: .file)
    }
}

nonisolated enum SimulatorStorageError: LocalizedError {
    case invalidResponse(String)
    var errorDescription: String? {
        switch self {
        case .invalidResponse(let detail): "存储服务返回了无法识别的结果：\(detail)"
        }
    }
}
