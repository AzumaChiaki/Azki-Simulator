import Foundation
import Darwin

/// Host metadata and requested public synthetic identity; never pairing keys.
nonisolated struct SimulatorInstanceRecord: Codable, Equatable, Identifiable, Sendable {
    let id: UUID
    var name: String
    var model: String
    var selectedProfileID: String?
    var virtualIdentity: SimulatorVirtualIdentity?
    var skipOOBE: Bool?
    let createdAt: Date

    init(id: UUID = UUID(), name: String, model: String, selectedProfileID: String? = nil, createdAt: Date = Date(),
         virtualIdentity: SimulatorVirtualIdentity? = nil, skipOOBE: Bool? = nil) {
        self.id = id; self.name = name; self.model = model
        self.selectedProfileID = selectedProfileID; self.createdAt = createdAt
        self.virtualIdentity = virtualIdentity
        self.skipOOBE = skipOOBE
    }
}

nonisolated enum SimulatorInstanceStoreError: LocalizedError, Equatable, Sendable {
    case invalid(String)
    case unsafe(String)
    case io(String, Int32)
    var errorDescription: String? {
        switch self {
        case .invalid(let detail): "实例记录无效：\(detail)"
        case .unsafe(let detail): "实例记录存储已阻止不安全操作：\(detail)"
        case .io(let operation, let code): "实例记录\(operation)失败（errno \(code)）"
        }
    }
}

/// Serialize calls on this store, then take a nonblocking same-user disk lock.
/// No existing object's permissions are repaired, and malformed old data is
/// never replaced by save(). All I/O is bounded and anchored to a checked root FD.
nonisolated final class SimulatorInstanceRecordStore: @unchecked Sendable {
    static let maximumRecords = 128
    static let maximumFileBytes = 131_072
    static var defaultRootURL: URL {
        SimulatorDataLocations.instanceRoot
    }
    let rootURL: URL
    private let lock = NSLock()
    private var rootIdentity: Identity?
    private static let filename = "instances.json"
    private static let lockname = ".instances.lock"
    private struct Document: Codable { let version: Int; let records: [SimulatorInstanceRecord] }
    private struct Identity: Equatable {
        let device: dev_t
        let inode: ino_t
        init(_ info: stat) { device = info.st_dev; inode = info.st_ino }
    }
    private struct Stamp: Equatable {
        let identity: Identity
        let size: off_t
        let mode: mode_t
        let uid: uid_t
        let links: nlink_t
        let modifiedSeconds: Int
        let modifiedNanoseconds: Int
        let changedSeconds: Int
        let changedNanoseconds: Int
        init(_ info: stat) {
            identity = Identity(info); size = info.st_size; mode = info.st_mode; uid = info.st_uid; links = info.st_nlink
            modifiedSeconds = info.st_mtimespec.tv_sec; modifiedNanoseconds = info.st_mtimespec.tv_nsec
            changedSeconds = info.st_ctimespec.tv_sec; changedNanoseconds = info.st_ctimespec.tv_nsec
        }
    }
    private struct Existing { let stamp: Stamp; let records: [SimulatorInstanceRecord] }

    init(rootURL: URL = SimulatorInstanceRecordStore.defaultRootURL) { self.rootURL = rootURL }

    func load() throws -> [SimulatorInstanceRecord] {
        lock.lock(); defer { lock.unlock() }
        let root = try openRoot(); defer { Darwin.close(root) }
        let diskLock = try acquireLock(root); defer { Darwin.close(diskLock) }
        let result = try readExisting(root)?.records ?? []
        try checkRoot(root)
        return result
    }

    func save(_ records: [SimulatorInstanceRecord]) throws {
        try Self.validate(records)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        // Codable's reference-date seconds preserve Date precision on round trip.
        let data = try encoder.encode(Document(version: 1, records: records))
        guard data.count <= Self.maximumFileBytes else { throw SimulatorInstanceStoreError.invalid("文件超过 128 KiB") }
        lock.lock(); defer { lock.unlock() }
        let root = try openRoot(); defer { Darwin.close(root) }
        let diskLock = try acquireLock(root); defer { Darwin.close(diskLock) }
        // Decode/validate the previous document before any replacement. A failed
        // load must not turn into an implicit reset on the caller's next save.
        let previous = try readExisting(root)
        let temporary = ".instances-" + UUID().uuidString.lowercased() + ".tmp"
        let fd = openat(root, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw Self.io("创建临时文件") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0 else { throw Self.io("检查临时文件") }
        let temporaryIdentity = Identity(info)
        defer {
            var current = stat()
            if fstatat(root, temporary, &current, AT_SYMLINK_NOFOLLOW) == 0,
               Identity(current) == temporaryIdentity { _ = unlinkat(root, temporary, 0) }
        }
        try Self.checkFile(info)
        try data.withUnsafeBytes { bytes in
            var position = 0
            while position < bytes.count {
                let count = Darwin.write(fd, bytes.baseAddress!.advanced(by: position), bytes.count - position)
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw Self.io("写入") }
                position += count
            }
        }
        guard fsync(fd) == 0 else { throw Self.io("同步文件") }
        try checkRoot(root)
        let current = try Self.fileStamp(root, name: Self.filename)
        guard current == previous?.stamp else { throw SimulatorInstanceStoreError.unsafe("原记录在保存期间被替换或修改") }
        var staged = stat()
        guard fstatat(root, temporary, &staged, AT_SYMLINK_NOFOLLOW) == 0,
              Identity(staged) == temporaryIdentity, staged.st_size == data.count else {
            throw SimulatorInstanceStoreError.unsafe("临时文件在保存期间改变")
        }
        try Self.checkFile(staged)
        let result = renameatx_np(root, temporary, root, Self.filename, previous == nil ? UInt32(RENAME_EXCL) : 0)
        guard result == 0 else { throw Self.io("原子保存") }
        guard fsync(root) == 0 else { throw Self.io("同步目录") }
        try checkRoot(root)
    }

    private static func validate(_ records: [SimulatorInstanceRecord]) throws {
        guard records.count <= maximumRecords else { throw SimulatorInstanceStoreError.invalid("最多保存 128 个实例") }
        var ids = Set<UUID>()
        for record in records {
            guard ids.insert(record.id).inserted else { throw SimulatorInstanceStoreError.invalid("实例 ID 重复") }
            let name = record.name
            guard (1...128).contains(name.utf8.count), name == name.trimmingCharacters(in: .whitespacesAndNewlines),
                  name != ".", name != "..", !name.contains(where: { "/\\:".contains($0) }),
                  !name.unicodeScalars.contains(where: {
                      CharacterSet.controlCharacters.contains($0) || $0.properties.generalCategory == .format
                  }) else { throw SimulatorInstanceStoreError.invalid("名称须为 1–128 字节可见文字，不能包含控制符或路径分隔符") }
            guard (1...96).contains(record.model.utf8.count),
                  record.model.range(of: #"^[a-z0-9][a-z0-9._-]*$"#, options: .regularExpression) != nil,
                  !record.model.contains("..") else { throw SimulatorInstanceStoreError.invalid("型号须保留目录中的准确标识") }
            if let profile = record.selectedProfileID {
                guard (1...128).contains(profile.utf8.count),
                      profile.range(of: #"^[a-z0-9][a-z0-9._-]*@[0-9]+(?:\.[0-9]+)*(?:@[a-f0-9]{12})?$"#, options: .regularExpression) != nil,
                      !profile.contains("..") else { throw SimulatorInstanceStoreError.invalid("固件配置标识无效") }
            }
            if let identity = record.virtualIdentity {
                guard try identity.validated(model: record.model) == identity else {
                    throw SimulatorInstanceStoreError.invalid("虚拟身份格式未规范化")
                }
            }
            guard record.createdAt.timeIntervalSince1970.isFinite,
                  (0...253_402_300_799).contains(record.createdAt.timeIntervalSince1970) else {
                throw SimulatorInstanceStoreError.invalid("创建时间无效")
            }
        }
    }

    private func openRoot() throws -> Int32 {
        let path = rootURL.path
        guard rootURL.isFileURL, path.hasPrefix("/"), path.utf8.count < 4_096, !path.contains("\0"),
              path != "/", path.dropFirst().split(separator: "/", omittingEmptySubsequences: false)
                .allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
            throw SimulatorInstanceStoreError.unsafe("根目录路径无效")
        }
        if rootIdentity != nil {
            let fd = Darwin.open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
            guard fd >= 0 else { throw SimulatorInstanceStoreError.unsafe("已使用的根目录消失、被替换或包含链接") }
            do { try checkRoot(fd); return fd } catch { Darwin.close(fd); throw error }
        }
        let fd = try Self.ensureDirectory(rootURL, privateOnly: true)
        do {
            var info = stat()
            guard fstat(fd, &info) == 0 else { throw Self.io("检查根目录") }
            rootIdentity = Identity(info)
            try checkRoot(fd)
            return fd
        } catch { Darwin.close(fd); throw error }
    }

    private static func ensureDirectory(_ url: URL, privateOnly: Bool) throws -> Int32 {
        var fd = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        if fd < 0 {
            guard errno == ENOENT, url.path != "/" else { throw SimulatorInstanceStoreError.unsafe("目录不可用或包含符号链接") }
            let parentURL = url.deletingLastPathComponent()
            let parent = try ensureDirectory(parentURL, privateOnly: false)
            defer { Darwin.close(parent) }
            if mkdirat(parent, url.lastPathComponent, 0o700) != 0, errno != EEXIST { throw io("创建目录") }
            fd = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        }
        guard fd >= 0 else { throw SimulatorInstanceStoreError.unsafe("无法安全打开目录") }
        var info = stat()
        guard fstat(fd, &info) == 0 else { Darwin.close(fd); throw SimulatorInstanceStoreError.unsafe("无法检查目录") }
        // A restore, a Finder copy or an installer can widen this directory's
        // mode. Narrow it back instead of refusing to open the instance store;
        // foreign owners and group/other-writable modes are still rejected.
        if privateOnly, info.st_mode & S_IFMT == S_IFDIR, info.st_uid == geteuid(),
           info.st_mode & 0o022 == 0, info.st_mode & 0o7777 != 0o700 {
            _ = fchmod(fd, 0o700)
            guard fstat(fd, &info) == 0 else { Darwin.close(fd); throw SimulatorInstanceStoreError.unsafe("无法检查目录") }
        }
        let valid = info.st_mode & S_IFMT == S_IFDIR
            && (privateOnly ? info.st_uid == geteuid() && info.st_mode & 0o7777 == 0o700
                : (info.st_uid == geteuid() || info.st_uid == 0)
                    && (info.st_mode & 0o022 == 0 || (info.st_uid == 0 && info.st_mode & S_ISVTX != 0)))
        guard valid else { Darwin.close(fd); throw SimulatorInstanceStoreError.unsafe("目录所有者或权限不符合要求，实例根目录必须为 0700") }
        return fd
    }

    private func checkRoot(_ held: Int32) throws {
        let current = Darwin.open(rootURL.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard current >= 0 else { throw SimulatorInstanceStoreError.unsafe("根目录路径改变或包含链接") }
        defer { Darwin.close(current) }
        var pinned = stat(), visible = stat()
        guard fstat(held, &pinned) == 0, fstat(current, &visible) == 0,
              Identity(pinned) == rootIdentity, Identity(visible) == rootIdentity,
              pinned.st_uid == geteuid(), pinned.st_mode & 0o7777 == 0o700 else {
            throw SimulatorInstanceStoreError.unsafe("根目录身份或权限发生变化")
        }
    }

    private func acquireLock(_ root: Int32) throws -> Int32 {
        let fd = openat(root, Self.lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw Self.io("打开锁") }
        do {
            var info = stat()
            guard fstat(fd, &info) == 0 else { throw Self.io("检查锁") }
            if info.st_uid == geteuid(), info.st_mode & 0o022 == 0, info.st_mode & 0o7777 != 0o600 {
                _ = fchmod(fd, 0o600)
                guard fstat(fd, &info) == 0 else { throw Self.io("检查锁") }
            }
            try Self.checkFile(info)
            guard info.st_size == 0 else { throw SimulatorInstanceStoreError.unsafe("锁文件含异常内容") }
            guard flock(fd, LOCK_EX | LOCK_NB) == 0 else { throw Self.io("取得独占锁") }
            try checkRoot(root)
            guard try Self.fileStamp(root, name: Self.lockname)?.identity == Identity(info) else {
                throw SimulatorInstanceStoreError.unsafe("锁文件被替换")
            }
            return fd
        } catch { Darwin.close(fd); throw error }
    }

    private func readExisting(_ root: Int32) throws -> Existing? {
        guard let before = try Self.fileStamp(root, name: Self.filename) else { return nil }
        let fd = openat(root, Self.filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard fd >= 0 else { throw Self.io("读取") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, Stamp(info) == before else { throw SimulatorInstanceStoreError.unsafe("记录在打开期间发生变化") }
        var data = Data(), buffer = [UInt8](repeating: 0, count: 8_192)
        while true {
            let count = Darwin.read(fd, &buffer, min(buffer.count, Self.maximumFileBytes + 1 - data.count))
            if count < 0, errno == EINTR { continue }
            guard count >= 0 else { throw Self.io("读取") }
            if count == 0 { break }
            data.append(contentsOf: buffer.prefix(count))
            guard data.count <= Self.maximumFileBytes else { throw SimulatorInstanceStoreError.invalid("文件超过 128 KiB") }
        }
        guard fstat(fd, &info) == 0, Stamp(info) == before, data.count == before.size,
              try Self.fileStamp(root, name: Self.filename) == before else {
            throw SimulatorInstanceStoreError.unsafe("记录在读取期间发生变化")
        }
        let document: Document
        do {
            guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  Set(object.keys) == ["version", "records"], let rows = object["records"] as? [[String: Any]],
                  rows.count <= Self.maximumRecords, rows.allSatisfy({ row in
                      Set(row.keys).isSuperset(of: ["id", "name", "model", "createdAt"])
                        && Set(row.keys).isSubset(of: ["id", "name", "model", "selectedProfileID", "createdAt", "virtualIdentity", "skipOOBE"])
                  }) else { throw SimulatorInstanceStoreError.invalid("文档字段不匹配") }
            document = try JSONDecoder().decode(Document.self, from: data)
        } catch { throw SimulatorInstanceStoreError.invalid("现有记录无法完整解析；已保留原文件") }
        guard document.version == 1 else { throw SimulatorInstanceStoreError.invalid("记录格式版本不受支持") }
        try Self.validate(document.records)
        return Existing(stamp: before, records: document.records)
    }

    private static func fileStamp(_ root: Int32, name: String) throws -> Stamp? {
        var info = stat()
        guard fstatat(root, name, &info, AT_SYMLINK_NOFOLLOW) == 0 else {
            if errno == ENOENT { return nil }
            throw io("检查文件")
        }
        // The document may have been restored with a wider mode; narrow it back
        // rather than refusing to read the user's own instance list.
        if info.st_mode & S_IFMT == S_IFREG, info.st_uid == geteuid(), info.st_nlink == 1,
           info.st_mode & 0o022 == 0, info.st_mode & 0o7777 != 0o600 {
            let fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
            if fd >= 0 {
                _ = fchmod(fd, 0o600)
                Darwin.close(fd)
                guard fstatat(root, name, &info, AT_SYMLINK_NOFOLLOW) == 0 else { throw io("检查文件") }
            }
        }
        try checkFile(info)
        return Stamp(info)
    }

    private static func checkFile(_ info: stat) throws {
        guard info.st_mode & S_IFMT == S_IFREG, info.st_uid == geteuid(), info.st_mode & 0o7777 == 0o600,
              info.st_nlink == 1, info.st_size >= 0, info.st_size <= maximumFileBytes else {
            throw SimulatorInstanceStoreError.unsafe("文件必须为当前用户的 0600 单链接常规文件，且不超过 128 KiB")
        }
    }

    private static func io(_ operation: String) -> SimulatorInstanceStoreError { .io(operation, errno) }
}
