import CryptoKit
import Darwin
import Foundation

/// A host-side copy only. The runtime must independently validate this file
/// before using it; this receipt does not establish guest installation.
nonisolated struct SimulatorStagedPackage: Sendable, Equatable {
    let fileName: String
    let sha256: String
    let byteCount: Int
}

nonisolated enum SimulatorPackageImport {
    static let maximumBytes = 32 * 1_024 * 1_024

    /// Streaming, synchronous I/O. Call from the session connection's executor
    /// or a background task, never the main actor.
    static func stage(source: URL, sessionDirectory: URL) throws -> SimulatorStagedPackage {
        try copy(source: source, sessionDirectory: sessionDirectory, identifier: UUID(), afterChunk: nil)
    }

    #if DEBUG
    // Deterministic filesystem-race coverage without changing production callers.
    static func stageForTesting(source: URL, sessionDirectory: URL, identifier: UUID = UUID(),
                                afterChunk: (() throws -> Void)? = nil) throws -> SimulatorStagedPackage {
        try copy(source: source, sessionDirectory: sessionDirectory, identifier: identifier, afterChunk: afterChunk)
    }
    #endif

    private static func copy(source: URL, sessionDirectory: URL, identifier: UUID,
                             afterChunk: (() throws -> Void)?) throws -> SimulatorStagedPackage {
        guard isLocalFileURL(source), isLocalFileURL(sessionDirectory) else { throw unsafe("请选择本地文件和会话目录") }
        _ = try StandaloneSandboxFiles.canonicalURL(source.path)
        _ = try StandaloneSandboxFiles.canonicalURL(sessionDirectory.path)
        try Task.checkCancellation()
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }

        let input = Darwin.open(source.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard input >= 0 else { throw unsafe("导入文件不可读或包含符号链接") }
        defer { Darwin.close(input) }
        let before = try sourceInfo(input)

        let root = Darwin.open(sessionDirectory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard root >= 0 else { throw unsafe("会话目录不可用或包含符号链接") }
        defer { Darwin.close(root) }
        let rootInfo = try privateDirectory(root)
        if mkdirat(root, "incoming", 0o700) != 0, errno != EEXIST { throw unsafe("无法创建导入目录") }
        let incoming = openat(root, "incoming", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard incoming >= 0 else { throw unsafe("导入目录不可用或包含符号链接") }
        defer { Darwin.close(incoming) }
        let incomingInfo = try privateDirectory(incoming)
        try checkDirectories(sessionDirectory, root: root, rootInfo: rootInfo,
                             incoming: incoming, incomingInfo: incomingInfo)

        let fileName = identifier.uuidString.lowercased() + ".bin"
        let output = openat(incoming, fileName, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard output >= 0 else { throw unsafe("无法独占创建导入文件") }
        defer { Darwin.close(output) }
        var outputIdentity = stat()
        guard fstat(output, &outputIdentity) == 0 else { throw unsafe("无法检查导入文件") }
        var completed = false
        defer {
            // Remove only our own inode, even if another writer replaces the name.
            if !completed {
                var visible = stat()
                if fstatat(incoming, fileName, &visible, AT_SYMLINK_NOFOLLOW) == 0,
                   sameIdentity(visible, outputIdentity) { _ = unlinkat(incoming, fileName, 0) }
            }
        }
        try validateOutput(outputIdentity, size: 0)
        var hasher = SHA256(), count = 0
        var buffer = [UInt8](repeating: 0, count: 65_536)
        while true {
            try Task.checkCancellation()
            let length = Darwin.read(input, &buffer, min(buffer.count, maximumBytes - count + 1))
            if length < 0, errno == EINTR { continue }
            guard length >= 0 else { throw unsafe("导入文件读取失败") }
            if length == 0 { break }
            count += length
            guard count <= maximumBytes else { throw unsafe("导入文件超过 32 MiB") }
            try buffer.withUnsafeBytes { raw in
                hasher.update(bufferPointer: UnsafeRawBufferPointer(rebasing: raw.prefix(length)))
                var offset = 0
                while offset < length {
                    try Task.checkCancellation()
                    let written = Darwin.write(output, raw.baseAddress!.advanced(by: offset), length - offset)
                    if written < 0, errno == EINTR { continue }
                    guard written > 0 else { throw unsafe("导入文件写入失败") }
                    offset += written
                }
            }
            try afterChunk?()
        }
        let after = try sourceInfo(input)
        guard sameSource(before, after), count == Int(before.st_size) else {
            throw unsafe("源文件在复制过程中发生变化")
        }
        // Also reject a replaced pathname instead of returning a receipt for a
        // now-unselected inode; O_NOFOLLOW_ANY rechecks every path component.
        let selected = Darwin.open(source.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard selected >= 0 else { throw unsafe("源文件路径在复制过程中发生变化") }
        defer { Darwin.close(selected) }
        guard try sameSource(before, sourceInfo(selected)) else { throw unsafe("源文件路径在复制过程中发生变化") }
        try synchronize(output)
        var finalOutput = stat(), visible = stat()
        guard fstat(output, &finalOutput) == 0,
              fstatat(incoming, fileName, &visible, AT_SYMLINK_NOFOLLOW) == 0,
              sameIdentity(outputIdentity, finalOutput), sameSource(finalOutput, visible) else {
            throw unsafe("导入文件在复制过程中被替换")
        }
        try validateOutput(finalOutput, size: count)
        try checkDirectories(sessionDirectory, root: root, rootInfo: rootInfo,
                             incoming: incoming, incomingInfo: incomingInfo)
        try synchronize(incoming)
        try synchronize(root)
        try Task.checkCancellation()
        completed = true
        return SimulatorStagedPackage(fileName: fileName,
            sha256: hasher.finalize().map { String(format: "%02x", $0) }.joined(), byteCount: count)
    }

    private static func sourceInfo(_ fd: Int32) throws -> stat {
        var value = stat()
        guard fstat(fd, &value) == 0, value.st_mode & S_IFMT == S_IFREG, value.st_nlink == 1,
              value.st_uid == geteuid() || value.st_uid == 0, value.st_mode & 0o022 == 0,
              value.st_size >= 0, value.st_size <= maximumBytes else {
            throw unsafe("源文件必须为不超过 32 MiB 的普通文件，且所有者、链接数和写入权限须符合要求")
        }
        return value
    }

    private static func isLocalFileURL(_ url: URL) -> Bool {
        url.isFileURL && (url.host == nil || url.host == "" || url.host == "localhost")
    }

    private static func privateDirectory(_ fd: Int32) throws -> stat {
        var value = stat()
        guard fstat(fd, &value) == 0, value.st_mode & S_IFMT == S_IFDIR,
              value.st_uid == geteuid(), value.st_mode & 0o7777 == 0o700 else {
            throw unsafe("会话和导入目录须属于当前用户且权限为 0700")
        }
        return value
    }

    private static func checkDirectories(_ url: URL, root: Int32, rootInfo: stat,
                                          incoming: Int32, incomingInfo: stat) throws {
        let visibleRoot = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard visibleRoot >= 0 else { throw unsafe("会话目录路径发生变化") }
        defer { Darwin.close(visibleRoot) }
        let visibleIncoming = openat(root, "incoming", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard visibleIncoming >= 0 else { throw unsafe("导入目录路径发生变化") }
        defer { Darwin.close(visibleIncoming) }
        guard try sameIdentity(rootInfo, privateDirectory(root)),
              try sameIdentity(rootInfo, privateDirectory(visibleRoot)),
              try sameIdentity(incomingInfo, privateDirectory(incoming)),
              try sameIdentity(incomingInfo, privateDirectory(visibleIncoming)) else {
            throw unsafe("会话或导入目录被替换")
        }
    }

    private static func validateOutput(_ value: stat, size: Int) throws {
        guard value.st_mode & S_IFMT == S_IFREG, value.st_nlink == 1,
              value.st_uid == geteuid(), value.st_mode & 0o7777 == 0o600, value.st_size == size else {
            throw unsafe("导入文件属性发生变化")
        }
    }

    private static func sameIdentity(_ lhs: stat, _ rhs: stat) -> Bool {
        lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino
    }

    private static func sameSource(_ lhs: stat, _ rhs: stat) -> Bool {
        sameIdentity(lhs, rhs) && lhs.st_size == rhs.st_size && lhs.st_mode == rhs.st_mode
            && lhs.st_uid == rhs.st_uid && lhs.st_gid == rhs.st_gid && lhs.st_nlink == rhs.st_nlink
            && lhs.st_mtimespec.tv_sec == rhs.st_mtimespec.tv_sec && lhs.st_mtimespec.tv_nsec == rhs.st_mtimespec.tv_nsec
            && lhs.st_ctimespec.tv_sec == rhs.st_ctimespec.tv_sec && lhs.st_ctimespec.tv_nsec == rhs.st_ctimespec.tv_nsec
    }

    private static func synchronize(_ fd: Int32) throws {
        while fsync(fd) != 0 {
            if errno == EINTR { try Task.checkCancellation(); continue }
            throw unsafe("导入文件同步失败")
        }
    }

    private static func unsafe(_ message: String) -> SimulatorError { .unsafeFile(message) }
}
