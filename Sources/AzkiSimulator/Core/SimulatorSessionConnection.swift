import Foundation
import CoreGraphics
import Darwin

/// A user-selected grant exists only for the attached session. No bookmark or
/// access token is written to preferences, Keychain or disk.
nonisolated fileprivate final class SimulatorSelectedDirectory: @unchecked Sendable {
    let url: URL
    let bookmark: Data?
    private let scoped: Bool

    init(_ url: URL) throws {
        guard url.isFileURL else { throw SimulatorError.invalidSession("请选择本机会话文件夹") }
        self.url = url
        scoped = url.startAccessingSecurityScopedResource()
        bookmark = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
    }

    deinit { if scoped { url.stopAccessingSecurityScopedResource() } }
}

/// All file/socket I/O runs on this actor's executor, never in a drawing callback
/// or on the main actor. Each command uses one bounded, local Unix connection.
actor SimulatorSessionConnection {
    nonisolated let manifest: SimulatorSessionManifest
    private var directory: SimulatorSelectedDirectory?
    private var directoryFD: Int32
    private let rootPath: String
    private let rootDevice: dev_t
    private let rootInode: ino_t
    private var activeInput: SimulatorInputEvent?
    private var closed = false

    init(directoryURL: URL) throws {
        let grant = try SimulatorSelectedDirectory(directoryURL)
        let root = directoryURL.path
        let fd = Darwin.open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("无法打开会话文件夹，或目录为符号链接") }
        do {
            var metadata = stat()
            guard fstat(fd, &metadata) == 0, metadata.st_uid == getuid(), metadata.st_mode & 0o022 == 0 else {
                throw SimulatorError.unsafeFile("会话文件夹须属于当前用户，且不能允许其他用户写入")
            }
            let bytes = try Self.readFile(directoryFD: fd, name: "session.json", limit: 16_384)
            manifest = try SimulatorSessionManifest.decode(bytes)
            directory = grant
            directoryFD = fd
            rootPath = root
            rootDevice = metadata.st_dev
            rootInode = metadata.st_ino
        } catch {
            Darwin.close(fd)
            throw error
        }
    }

    deinit { if directoryFD >= 0 { Darwin.close(directoryFD) } }

    func frame(after sequence: UInt32?) throws -> SimulatorFrame? {
        guard !closed else { throw SimulatorError.disconnected }
        return try Self.readFrame(directoryFD: directoryFD, manifest: manifest, after: sequence)
    }

    /// A separate executor keeps bounded but slow control/log socket requests
    /// from stalling frame reads. Duplicate the already validated directory FD;
    /// do not resolve a second path or acquire a different session identity.
    func makeFrameReader() throws -> SimulatorFrameReader {
        guard !closed, let directory else { throw SimulatorError.disconnected }
        let fd = dup(directoryFD)
        guard fd >= 0 else { throw SimulatorError.transport("无法复制画面目录描述符 (errno \(errno))") }
        guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 else {
            let code = errno
            Darwin.close(fd)
            throw SimulatorError.transport("无法保护画面目录描述符 (errno \(code))")
        }
        return SimulatorFrameReader(directory: directory, directoryFD: fd, manifest: manifest)
    }

    fileprivate nonisolated static func readFrame(directoryFD: Int32, manifest: SimulatorSessionManifest,
                                                  after sequence: UInt32?) throws -> SimulatorFrame? {
        let expectedBytes = SimulatorFrameHeader.byteCount + manifest.width * manifest.height * 2
        let fd = try Self.openRegularFile(directoryFD: directoryFD, name: manifest.framebuffer,
                                         limit: expectedBytes, allowMissingFrame: true)
        // ENOENT is normal before the first frame and across stop/republication.
        // All other open failures and unsafe existing files still fail closed.
        guard fd >= 0 else { return nil }
        defer { Darwin.close(fd) }
        let first = try Self.readExactly(fd, count: SimulatorFrameHeader.byteCount, offset: 0)
        let header = try SimulatorFrameHeader.decode(first)
        var fileInfo = stat()
        guard fstat(fd, &fileInfo) == 0, fileInfo.st_size == header.byteCount else {
            throw SimulatorError.invalidFrame("画面文件长度不匹配")
        }
        guard header.width == manifest.width, header.height == manifest.height, header.stride == manifest.width * 2 else {
            throw SimulatorError.invalidFrame("尺寸与会话不一致")
        }
        if sequence == header.sequence { return nil }
        let bytes = try Self.readExactly(fd, count: header.byteCount, offset: 0)
        let last = try Self.readExactly(fd, count: SimulatorFrameHeader.byteCount, offset: 0)
        guard first == last, bytes.prefix(SimulatorFrameHeader.byteCount) == first else {
            throw SimulatorError.frameBeingWritten
        }
        return try SimulatorFrame.decode(bytes, expectedSize: CGSize(width: manifest.width, height: manifest.height))
    }

    func command(_ command: String, arguments: [String: SimulatorJSON] = [:]) throws -> SimulatorJSON {
        guard !closed else { throw SimulatorError.disconnected }
        let permitted = ["status", "start", "stop", "input", "screenshot", "logs", "inspect-package", "stage-package", "pmu-adc-get", "pmu-adc-set",
                         "guest-status", "guest-connect", "guest-install", "guest-operation", "guest-inventory", "guest-select",
                         "external-open", "external-status", "external-request", "external-result", "external-close", "external-reconnect"]
        guard permitted.contains(command) else { throw SimulatorError.remote("不受支持的会话命令") }
        if command == "stop" { releaseInput() }
        return try exchange(command, arguments: arguments)
    }

    func installGuestPackage(_ source: URL, expectedSessionID: String, expectedGeneration: Int) throws -> SimulatorJSON {
        guard !closed else { throw SimulatorError.disconnected }
        let root = URL(fileURLWithPath: rootPath, isDirectory: true)
        let metadata = try StandaloneSandboxFiles.directory(root, privateOnly: true)
        guard metadata.st_dev == rootDevice, metadata.st_ino == rootInode else {
            throw SimulatorError.unsafeFile("会话目录已被替换")
        }
        let package = try SimulatorPackageImport.stage(source: source, sessionDirectory: root)
        let after = try StandaloneSandboxFiles.directory(root, privateOnly: true)
        guard after.st_dev == rootDevice, after.st_ino == rootInode else {
            throw SimulatorError.unsafeFile("导入期间会话目录已被替换")
        }
        return try command("guest-install", arguments: [
            "fileName": .string(package.fileName), "sha256": .string(package.sha256),
            "expected_session_id": .string(expectedSessionID),
            "expected_generation": .number(Double(expectedGeneration))])
    }

    func input(_ event: SimulatorInputEvent) throws {
        guard !closed else { throw SimulatorError.disconnected }
        guard event.x >= 0, event.x < manifest.width, event.y >= 0, event.y < manifest.height else {
            throw SimulatorError.remote("触摸坐标超出设备画面")
        }
        // Track a down event before sending: even a lost acknowledgement must
        // result in an attempted release when the view disappears.
        if event.down { activeInput = event }
        _ = try exchange("input", arguments: ["x": .number(Double(event.x)), "y": .number(Double(event.y)), "down": .bool(event.down)])
        if !event.down { activeInput = nil }
    }

    func close() {
        guard !closed else { return }
        closed = true
        releaseInput()
        Darwin.close(directoryFD)
        directoryFD = -1
        directory = nil
    }

    private func releaseInput() {
        guard let event = activeInput else { return }
        _ = try? exchange("input", arguments: ["x": .number(Double(event.x)), "y": .number(Double(event.y)), "down": .bool(false)])
        activeInput = nil
    }

    private func exchange(_ command: String, arguments: [String: SimulatorJSON]) throws -> SimulatorJSON {
        let timeout = ConnectionSettings.readTimeoutSeconds()
        let deadline = ProcessInfo.processInfo.systemUptime + Double(timeout)
        var arguments = arguments
        if command.hasPrefix("guest-") {
            arguments["connectionTimeoutSeconds"] = .number(Double(timeout))
        }
        try validateSocketLocation()
        let path = rootPath + "/" + manifest.socket
        let pathBytes = Array(path.utf8) + [0]
        var address = sockaddr_un()
        guard pathBytes.count <= MemoryLayout.size(ofValue: address.sun_path) else {
            throw SimulatorError.invalidSession("会话目录路径过长；请使用较短的本机路径")
        }
        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        withUnsafeMutableBytes(of: &address.sun_path) { target in target.copyBytes(from: pathBytes) }
        let socketFD = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard socketFD >= 0 else { throw SimulatorError.transport("无法创建本机 socket") }
        defer { Darwin.close(socketFD) }
        guard fcntl(socketFD, F_SETFD, FD_CLOEXEC) == 0,
              fcntl(socketFD, F_SETFL, O_NONBLOCK) == 0 else {
            throw SimulatorError.transport("无法设置非阻塞 socket")
        }
        var noSignal: Int32 = 1
        guard setsockopt(socketFD, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            throw SimulatorError.transport("无法设置 socket 错误隔离")
        }
        let result = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(socketFD, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        if result != 0 {
            let code = errno
            guard code == EINPROGRESS || code == EWOULDBLOCK || code == EINTR else {
                throw try connectionError(code)
            }
            try Self.waitForSocket(socketFD, events: Int16(POLLOUT), deadline: deadline)
            var socketError: Int32 = 0
            var length = socklen_t(MemoryLayout<Int32>.size)
            guard getsockopt(socketFD, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0, socketError == 0 else {
                throw try connectionError(socketError == 0 ? errno : socketError)
            }
        }
        var peerUID: uid_t = 0, peerGID: gid_t = 0
        guard getpeereid(socketFD, &peerUID, &peerGID) == 0, peerUID == getuid() else {
            throw SimulatorError.transport("socket 对端不属于当前用户")
        }
        try validateSocketLocation()
        var request = try JSONEncoder().encode(SimulatorBridgeRequest(id: manifest.id, command: command, arguments: arguments))
        guard request.count < 65_536 else { throw SimulatorError.transport("请求超出限制") }
        request.append(10)
        try request.withUnsafeBytes { bytes in
            var sent = 0
            while sent < bytes.count {
                try Self.waitForSocket(socketFD, events: Int16(POLLOUT), deadline: deadline)
                let count = Darwin.send(socketFD, bytes.baseAddress!.advanced(by: sent), bytes.count - sent, 0)
                if count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { continue }
                guard count > 0 else { throw SimulatorError.transport("请求发送失败") }
                sent += count
            }
        }
        var response = Data()
        var buffer = [UInt8](repeating: 0, count: 4_096)
        // The same absolute deadline includes connect, send and every receive,
        // even if a peer trickles a byte immediately before a timeout.
        while response.count <= 1_048_576, ProcessInfo.processInfo.systemUptime < deadline {
            try Self.waitForSocket(socketFD, events: Int16(POLLIN), deadline: deadline)
            let count = Darwin.recv(socketFD, &buffer, buffer.count, 0)
            if count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { continue }
            guard count > 0 else { throw SimulatorError.transport("响应中断") }
            response.append(contentsOf: buffer.prefix(count))
            if let newline = response.firstIndex(of: 10) {
                guard newline <= 1_048_576 else { break }
                let reply = try JSONDecoder().decode(SimulatorBridgeResponse.self, from: response.prefix(upTo: newline))
                if !reply.ok, ["timeout", "guest_timeout"].contains(reply.error?.code ?? "") {
                    throw SimulatorConnectionTimeout()
                }
                return try reply.checkedResult()
            }
        }
        if ProcessInfo.processInfo.systemUptime >= deadline { throw SimulatorConnectionTimeout() }
        throw SimulatorError.transport("响应超出大小限制")
    }

    private nonisolated static func waitForSocket(_ fd: Int32, events: Int16, deadline: TimeInterval) throws {
        while true {
            let remaining = deadline - ProcessInfo.processInfo.systemUptime
            guard remaining > 0 else { throw SimulatorConnectionTimeout() }
            var descriptor = pollfd(fd: fd, events: events, revents: 0)
            let result = Darwin.poll(&descriptor, 1, Int32(max(1, min(20_000, ceil(remaining * 1_000)))))
            if result < 0, errno == EINTR { continue }
            guard result >= 0 else { throw SimulatorError.transport(String(cString: strerror(errno))) }
            guard result != 0 else { continue }
            guard descriptor.revents & Int16(POLLNVAL) == 0 else { throw SimulatorError.disconnected }
            if descriptor.revents & (events | Int16(POLLERR) | Int16(POLLHUP)) != 0 { return }
        }
    }

    private func validateSocketLocation() throws {
        var root = stat(), socketMetadata = stat()
        guard lstat(rootPath, &root) == 0 else {
            if errno == ENOENT { throw SimulatorError.disconnected }
            throw SimulatorError.unsafeFile("无法重新核验会话根目录")
        }
        guard root.st_mode & S_IFMT == S_IFDIR,
              root.st_dev == rootDevice, root.st_ino == rootInode,
              root.st_uid == getuid(), root.st_mode & 0o022 == 0 else {
            throw SimulatorError.unsafeFile("会话根目录的类型、身份或权限已改变")
        }
        guard fstatat(directoryFD, manifest.socket, &socketMetadata, AT_SYMLINK_NOFOLLOW) == 0 else {
            // The owned helper unlinks its socket during normal bounded exit.
            // Existing symlinks, wrong node types and changed roots remain unsafe.
            if errno == ENOENT { throw SimulatorError.disconnected }
            throw SimulatorError.unsafeFile("无法重新核验会话控制通道")
        }
        guard socketMetadata.st_mode & S_IFMT == S_IFSOCK, socketMetadata.st_uid == getuid(),
              socketMetadata.st_mode & 0o077 == 0 else {
            throw SimulatorError.unsafeFile("控制通道必须是会话根目录内、仅当前用户可访问的 Unix socket")
        }
    }

    private func connectionError(_ code: Int32) throws -> SimulatorError {
        if code == ENOENT || code == ECONNREFUSED {
            // Do not hide an unsafe replacement racing the pre-connect check.
            try validateSocketLocation()
            return .disconnected
        }
        return .transport(String(cString: strerror(code)))
    }

    private nonisolated static func openRegularFile(directoryFD: Int32, name: String, limit: Int,
                                                   allowMissingFrame: Bool = false) throws -> Int32 {
        guard !name.contains("/"), name != ".", name != ".." else { throw SimulatorError.unsafeFile(name) }
        let fd = openat(directoryFD, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard fd >= 0 else {
            let code = errno
            if allowMissingFrame, name == "frame.raw", code == ENOENT { return -1 }
            throw SimulatorError.unsafeFile("无法读取 \(name) (errno \(code): \(String(cString: strerror(code))))")
        }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG,
              info.st_uid == getuid(), info.st_nlink == 1, info.st_mode & 0o022 == 0,
              info.st_size >= 0, info.st_size <= limit else {
            Darwin.close(fd)
            throw SimulatorError.unsafeFile("\(name) 类型、所有权或大小不符合要求")
        }
        return fd
    }

    private nonisolated static func readFile(directoryFD: Int32, name: String, limit: Int) throws -> Data {
        let fd = try openRegularFile(directoryFD: directoryFD, name: name, limit: limit)
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0 else { throw SimulatorError.unsafeFile(name) }
        return try readExactly(fd, count: Int(info.st_size), offset: 0)
    }

    private nonisolated static func readExactly(_ fd: Int32, count: Int, offset: Int) throws -> Data {
        var bytes = Data(count: count)
        try bytes.withUnsafeMutableBytes { target in
            var copied = 0
            while copied < count {
                let result = pread(fd, target.baseAddress!.advanced(by: copied), count - copied, off_t(offset + copied))
                if result < 0 && errno == EINTR { continue }
                guard result > 0 else { throw SimulatorError.invalidFrame("文件已截断或无法读取") }
                copied += result
            }
        }
        return bytes
    }
}

/// Owns only the display's read capability. Closing the command connection or
/// this reader never stops a guest; the model closes both when it detaches.
actor SimulatorFrameReader {
    private var directory: SimulatorSelectedDirectory?
    private var directoryFD: Int32
    private let manifest: SimulatorSessionManifest

    fileprivate init(directory: SimulatorSelectedDirectory, directoryFD: Int32, manifest: SimulatorSessionManifest) {
        self.directory = directory
        self.directoryFD = directoryFD
        self.manifest = manifest
    }

    deinit { if directoryFD >= 0 { Darwin.close(directoryFD) } }

    func presentation(after sequence: UInt32?) throws -> SimulatorFramePresentation? {
        guard directoryFD >= 0 else { throw SimulatorError.disconnected }
        guard let frame = try SimulatorSessionConnection.readFrame(directoryFD: directoryFD,
            manifest: manifest, after: sequence) else { return nil }
        return SimulatorFramePresentation(frame: frame, image: frame.makeCGImage())
    }

    func close() {
        if directoryFD >= 0 { Darwin.close(directoryFD) }
        directoryFD = -1
        directory = nil
    }
}
