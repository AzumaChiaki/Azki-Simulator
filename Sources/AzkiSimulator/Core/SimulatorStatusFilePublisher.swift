import Foundation
import Observation
import Darwin
import CryptoKit

/// Explicitly enabled host-only sharing. The UI must exclude runtime operations
/// while this model is sharing. No process, runtime or core is started here.
@MainActor @Observable
final class HostStatusSharingModel {
    private(set) var isSharing = false
    var isBusy: Bool { enabling || stoppingActive }
    private(set) var sessionURL: URL?
    var errorMessage: String?
    @ObservationIgnored private let publisher = SimulatorStatusFilePublisher()
    @ObservationIgnored private var observation: Task<Void, Never>?
    @ObservationIgnored private var pendingEnable: Task<URL, Error>?
    @ObservationIgnored private var stopping: Task<Bool, Never>?
    private var enabling = false
    private var stoppingActive = false
    private var generation: UInt64 = 0

    deinit { observation?.cancel(); pendingEnable?.cancel() }

    func enable() async {
        guard !isBusy, !isSharing else { return }
        enabling = true
        let token = generation
        defer { enabling = false; pendingEnable = nil }
        errorMessage = nil
        do {
            let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development"
            let task = Task { [publisher] in try await publisher.start(applicationVersion: version, receiveWatchfaces: true) }
            pendingEnable = task
            let url = try await task.value
            guard token == generation else { return }
            sessionURL = url
            isSharing = true
            observation = Task { @MainActor [weak self, publisher] in
                while !Task.isCancelled {
                    do { try await Task.sleep(for: .milliseconds(250)) } catch { return }
                    let failure = await publisher.failureMessage
                    if let failure {
                        guard let self else { return }
                        self.isSharing = false
                        self.errorMessage = failure
                        return
                    }
                }
            }
        } catch { if token == generation { errorMessage = error.localizedDescription } }
    }

    /// Call and await from the normal application termination hook. A crash or
    /// SIGKILL is handled by the client's finite lease, not claimed cleanup.
    @discardableResult func disable() async -> Bool {
        if let stopping { return await stopping.value }
        stoppingActive = true
        generation &+= 1
        observation?.cancel()
        observation = nil
        let pending = pendingEnable
        let task = Task { @MainActor [self] in
            if let pending { _ = await pending.result }
            let stopped = await publisher.stop()
            isSharing = false
            if stopped { sessionURL = nil }
            else { errorMessage = "共享已停止更新，但私有会话清理未完成；请重试停止" }
            stopping = nil
            stoppingActive = false
            return stopped
        }
        stopping = task
        return await task.value
    }
}

nonisolated enum SimulatorStatusPublisherError: LocalizedError {
    case invalid(String)
    case io(String, Int32)
    var errorDescription: String? {
        switch self {
        case .invalid(let message): return message
        case .io(let operation, let code): return "状态共享失败（\(operation)，errno \(code)）"
        }
    }
}

/// Original Swift publisher for a bounded, same-user file queue. Files and UID
/// are not code-signing authentication. It deliberately has no command dispatch
/// to any emulator service. The only log is this actor's own in-memory host.log.
actor SimulatorStatusFilePublisher {
    static let protocolName = "azki.simulator.status-files.v1"
    static let studioProtocolName = "azki.simulator.studio-files.v1"
    static let leaseSeconds = 5
    static let maximumSessionSeconds: TimeInterval = 1_800
    private var session: Session?
    private var pump: Task<Void, Never>?
    private(set) var failureMessage: String?

    /// The manual-poll option is internal and used only by deterministic tests.
    func start(applicationVersion: String, automaticPolling: Bool = true, receiveWatchfaces: Bool = false) throws -> URL {
        guard session == nil else { throw SimulatorStatusPublisherError.invalid("请先停止已有状态共享") }
        guard (1...64).contains(applicationVersion.utf8.count),
              applicationVersion.utf8.allSatisfy({ (0x21...0x7e).contains($0) }) else {
            throw SimulatorStatusPublisherError.invalid("应用版本格式无效")
        }
        failureMessage = nil
        let created = try Session.create(version: applicationVersion, receiveWatchfaces: receiveWatchfaces)
        session = created
        do {
            created.record("Host status sharing enabled; core not started.")
            try created.publishLease(now: Self.now)
            created.manifest = try StatusFiles.publish(created.root, name: "session.json", object: [
                "version": 1, "id": created.id, "directory": created.url.path,
                "application": "AzkiSimulator", "protocol": receiveWatchfaces ? Self.studioProtocolName : Self.protocolName,
                "transport": "file-queue", "capabilities": created.capabilities,
                "requests": "requests", "responses": "responses", "lease": "lease.json"
            ])
        } catch {
            _ = created.cleanup()
            session = nil
            throw error
        }
        if automaticPolling {
            pump = Task { [weak self] in
                while !Task.isCancelled {
                    do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
                    guard let self else { return }
                    await self.poll()
                }
            }
        }
        return created.url
    }

    func stop() -> Bool {
        pump?.cancel()
        pump = nil
        guard let session else { return true }
        session.record("Host status sharing stopped.")
        let clean = session.cleanup()
        if clean { self.session = nil }
        return clean
    }

    /// All work on a tick is bounded: <=64 entries per queue, <=8 requests,
    /// <=4 KiB request, <=64 KiB response, and <=4096 unique requests/session.
    func poll(now: Int = SimulatorStatusFilePublisher.now) {
        guard let session, !session.stopped else { return }
        do {
            try session.validate()
            // Never revive a lease after suspension or a delayed actor tick.
            guard now < session.expiresAt, now >= session.lastHeartbeat,
                  ProcessInfo.processInfo.systemUptime - session.startedUptime < Self.maximumSessionSeconds else {
                throw SimulatorStatusPublisherError.invalid("状态共享租约已过期，请重新启用")
            }
            if now > session.lastHeartbeat { try session.publishLease(now: now) }
            try session.pruneResponses(now: now)
            let names = try StatusFiles.names(session.requests)
            for name in names.sorted().prefix(8) {
                do { try session.process(name: name, now: now) }
                catch SimulatorStatusPublisherError.io(_, let code) where code == ENOENT { continue }
            }
        } catch {
            failureMessage = "状态共享已撤回：" + error.localizedDescription
            pump?.cancel()
            pump = nil
            _ = session.cleanup()
            // Retain ownership for retrying a partially completed cleanup.
        }
    }

    nonisolated static var now: Int { Int(Date().timeIntervalSince1970) }

    private final class Session {
        let id: String
        let name: String
        let url: URL
        let version: String
        let receiveWatchfaces: Bool
        var capabilities: [String] { receiveWatchfaces ? ["status", "logs", "receive-watchface"] : ["status", "logs"] }
        var receivedFiles: [String: StatusFiles.Identity] = [:]
        var receivedBytes = 0
        let parent: Int32
        let root: Int32
        let requests: Int32
        let responses: Int32
        let rootIdentity: StatusFiles.Identity
        let requestsIdentity: StatusFiles.Identity
        let responsesIdentity: StatusFiles.Identity
        let startedUptime = ProcessInfo.processInfo.systemUptime
        var manifest: StatusFiles.Identity?
        var lease: StatusFiles.Identity?
        var sequence = 0
        var expiresAt = 0
        var lastHeartbeat = 0
        var stopped = false
        var seen = Set<String>()
        var acceptedRequests: [String: StatusFiles.Identity] = [:]
        var publishedResponses: [String: (identity: StatusFiles.Identity, expires: Int)] = [:]
        var removedQueues = Set<String>()
        var removedRoot = false
        var log = ""
        var logTruncated = false

        init(id: String, name: String, version: String, receiveWatchfaces: Bool, parent: Int32, root: Int32, requests: Int32, responses: Int32) throws {
            self.id = id; self.name = name; self.version = version; self.receiveWatchfaces = receiveWatchfaces
            self.parent = parent; self.root = root; self.requests = requests; self.responses = responses
            url = URL(fileURLWithPath: "/private/tmp/" + name, isDirectory: true)
            rootIdentity = try StatusFiles.info(root)
            requestsIdentity = try StatusFiles.info(requests)
            responsesIdentity = try StatusFiles.info(responses)
        }

        deinit {
            // Last-resort normal teardown uses held directory descriptors only.
            _ = cleanup()
            for fd in [responses, requests, root, parent] { Darwin.close(fd) }
        }

        static func create(version: String, receiveWatchfaces: Bool) throws -> Session {
            let id = UUID().uuidString.lowercased(), name = "azki-host-" + id
            let parent = Darwin.open("/private/tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
            guard parent >= 0 else { throw StatusFiles.error("打开临时目录") }
            var root: Int32 = -1, requests: Int32 = -1, responses: Int32 = -1
            var complete = false, createdRoot = false
            defer {
                if !complete {
                    if root >= 0 { _ = unlinkat(root, "requests", AT_REMOVEDIR); _ = unlinkat(root, "responses", AT_REMOVEDIR) }
                    for fd in [responses, requests, root] where fd >= 0 { Darwin.close(fd) }
                    if createdRoot { _ = unlinkat(parent, name, AT_REMOVEDIR) }
                    Darwin.close(parent)
                }
            }
            guard mkdirat(parent, name, 0o700) == 0 else { throw StatusFiles.error("创建会话") }
            createdRoot = true
            root = try StatusFiles.openChild(parent, name: name)
            guard mkdirat(root, "requests", 0o700) == 0, mkdirat(root, "responses", 0o700) == 0 else {
                throw StatusFiles.error("创建队列")
            }
            requests = try StatusFiles.openChild(root, name: "requests")
            responses = try StatusFiles.openChild(root, name: "responses")
            let result = try Session(id: id, name: name, version: version, receiveWatchfaces: receiveWatchfaces, parent: parent, root: root, requests: requests, responses: responses)
            complete = true
            return result
        }

        func record(_ text: String) {
            log += "\(SimulatorStatusFilePublisher.now) \(text)\n"
            // All recorded messages are fixed ASCII; never echo request values.
            if log.utf8.count > 16_384 {
                log = String(log.suffix(16_384)); logTruncated = true
            }
        }

        func validate() throws {
            guard !stopped,
                  try StatusFiles.node(parent, name: name)?.sameNode(rootIdentity) == true,
                  try StatusFiles.node(root, name: "requests")?.sameNode(requestsIdentity) == true,
                  try StatusFiles.node(root, name: "responses")?.sameNode(responsesIdentity) == true,
                  let manifest, try StatusFiles.node(root, name: "session.json") == manifest,
                  let lease, try StatusFiles.node(root, name: "lease.json") == lease else {
                throw SimulatorStatusPublisherError.invalid("会话、清单或队列节点被更改")
            }
            for fd in [root, requests, responses] { try StatusFiles.privateDirectory(StatusFiles.info(fd)) }
        }

        func publishLease(now: Int) throws {
            sequence += 1
            lease = try StatusFiles.publish(root, name: "lease.json", object: [
                "version": 1, "id": id, "sequence": sequence, "expiresAt": now + SimulatorStatusFilePublisher.leaseSeconds
            ], replacing: lease)
            lastHeartbeat = now; expiresAt = now + SimulatorStatusFilePublisher.leaseSeconds
        }

        func process(name: String, now: Int) throws {
            // Temporary client publications are invisible until rename. They
            // remain client-owned, including when an interrupted writer exits.
            guard let node = try StatusFiles.node(requests, name: name) else { return }
            try StatusFiles.privateFile(node, maximum: 4_096)
            if name.hasPrefix(".") {
                guard now - node.modifiedSeconds <= 10 else {
                    throw SimulatorStatusPublisherError.invalid("队列含过期的客户端临时文件")
                }
                return
            }
            guard name.hasSuffix(".json"), let uuid = UUID(uuidString: String(name.dropLast(5))),
                  name == uuid.uuidString.lowercased() + ".json" else {
                throw SimulatorStatusPublisherError.invalid("队列含未知文件名")
            }
            let requestID = uuid.uuidString.lowercased()
            let (data, identity) = try StatusFiles.read(requests, name: name, maximum: 4_096)
            let value: StatusRequestJSON
            do { value = try StatusRequestJSON.decode(data, maximum: 4_096) }
            catch {
                record("Rejected malformed request JSON.")
                throw SimulatorStatusPublisherError.invalid("请求 JSON 无效")
            }
            guard let fields = value.object, Set(fields.keys) == ["version", "id", "requestID", "command", "arguments", "expiresAt"],
                  value["version"]?.integer == 1, value["id"]?.string == id,
                  value["requestID"]?.string == requestID,
                  let expires = value["expiresAt"]?.integer,
                  let command = value["command"]?.string, let arguments = value["arguments"]?.object else {
                throw SimulatorStatusPublisherError.invalid("请求关联或字段无效")
            }
            acceptedRequests[name] = identity
            guard now < expires, expires - now <= 10 else {
                try removeRequest(name, identity)
                record("Rejected expired request envelope.")
                return
            }
            guard !seen.contains(requestID) else {
                try removeRequest(name, identity)
                record("Rejected replayed request.")
                return
            }
            guard seen.count < 4_096 else { throw SimulatorStatusPublisherError.invalid("本次共享请求数量已达上限") }
            seen.insert(requestID)
            let payload: [String: Any]
            if command == "status", arguments.isEmpty {
                record("Status request served.")
                payload = ["ok": true, "result": ["state": "host-running", "coreState": "core-not-started", "coreExecuted": false,
                    "applicationVersion": version, "protocolVersion": 1, "capabilities": capabilities, "transport": "file-queue"]]
            } else if command == "logs", Set(arguments.keys) == ["name"], arguments["name"]?.string == "host.log" {
                record("Host log request served.")
                payload = ["ok": true, "result": ["name": "host.log", "text": log, "truncated": logTruncated]]
            } else if command == "receive-watchface", receiveWatchfaces {
                do {
                    payload = ["ok": true, "result": try receiveWatchface(arguments, expires: expires)]
                } catch {
                    record("Watchface reception rejected.")
                    payload = ["ok": false, "error": ["code": "watchface_rejected", "message": "Watchface transfer validation failed."]]
                }
            } else {
                record("Rejected unsupported command or arguments.")
                payload = ["ok": false, "error": ["code": ["status", "logs"].contains(command) ? "invalid_arguments" : "unsupported_command",
                                                     "message": "Only host status and host.log are available."]]
            }
            try validate()
            guard SimulatorStatusFilePublisher.now < expires else {
                try removeRequest(name, identity)
                return
            }
            let receipt = try StatusFiles.publish(responses, name: name, object: ["version": 1, "id": id,
                "requestID": requestID, "expiresAt": expires, "payload": payload])
            publishedResponses[name] = (receipt, expires)
            try removeRequest(name, identity)
        }

        /// The transfer terminates at the native host. This is deliberately
        /// not an installation receipt; no guest protocol is invoked here.
        func receiveWatchface(_ arguments: [String: StatusRequestJSON], expires: Int) throws -> [String: Any] {
            guard Set(arguments.keys) == ["file", "bytes", "sha256"],
                  let source = arguments["file"]?.string, source.hasPrefix("upload-"), source.hasSuffix(".bin"),
                  let uuid = UUID(uuidString: String(source.dropFirst(7).dropLast(4))),
                  source == "upload-" + uuid.uuidString.lowercased() + ".bin",
                  let lengthText = arguments["bytes"]?.string, let length = Int(lengthText),
                  length > 0, length <= 16 * 1024 * 1024,
                  receivedFiles.count < 8, receivedBytes + length <= 64 * 1024 * 1024,
                  let expected = arguments["sha256"]?.string else {
                throw SimulatorStatusPublisherError.invalid("表盘请求或额度无效")
            }
            let (data, identity) = try StatusFiles.read(root, name: source, maximum: 16 * 1024 * 1024)
            let hash = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
            guard data.count == length, hash == expected, SimulatorStatusFilePublisher.now < expires else {
                throw SimulatorStatusPublisherError.invalid("表盘长度、摘要或有效期无效")
            }
            try validate()
            let destination = "watchface-" + uuid.uuidString.lowercased() + ".bin"
            // Copy the checked bytes into a new host-owned inode. A retained
            // client descriptor cannot alter the accepted copy after receipt.
            let accepted = try StatusFiles.publishBytes(root, name: destination, data: data)
            receivedFiles[destination] = accepted
            receivedBytes += length
            try StatusFiles.remove(root, name: source, identity: identity)
            record("Watchface received: " + destination + "; guest installation unavailable.")
            return ["state": "received", "file": destination, "sha256": hash, "bytes": length, "installed": false]
        }

        func removeRequest(_ name: String, _ identity: StatusFiles.Identity) throws {
            try StatusFiles.remove(requests, name: name, identity: identity)
            acceptedRequests[name] = nil
        }

        func pruneResponses(now: Int) throws {
            let names = try StatusFiles.names(responses)
            guard names.allSatisfy({ publishedResponses[$0] != nil }) else {
                throw SimulatorStatusPublisherError.invalid("响应队列含非本次发布文件")
            }
            for (name, receipt) in publishedResponses {
                guard let current = try StatusFiles.node(responses, name: name) else {
                    publishedResponses[name] = nil
                    continue
                }
                guard current == receipt.identity else { throw SimulatorStatusPublisherError.invalid("响应文件被更改") }
                if now >= receipt.expires {
                    try StatusFiles.remove(responses, name: name, identity: receipt.identity)
                    publishedResponses[name] = nil
                }
            }
        }

        func cleanup() -> Bool {
            stopped = true
            var clean = true
            for (name, identity) in receivedFiles {
                do { try StatusFiles.remove(root, name: name, identity: identity) }
                catch { clean = false }
            }
            for (name, identity) in [("session.json", manifest), ("lease.json", lease)] {
                if let identity {
                    do { try StatusFiles.remove(root, name: name, identity: identity) }
                    catch { clean = false }
                }
            }
            for (fd, name, identity) in [(requests, "requests", requestsIdentity), (responses, "responses", responsesIdentity)] {
                do {
                    let owned = name == "requests" ? acceptedRequests : publishedResponses.mapValues { $0.identity }
                    for (leaf, recorded) in owned {
                        try StatusFiles.remove(fd, name: leaf, identity: recorded)
                    }
                    if !removedQueues.contains(name) {
                        guard try StatusFiles.node(root, name: name)?.sameNode(identity) == true else {
                            throw SimulatorStatusPublisherError.invalid("清理时队列节点被替换")
                        }
                        guard unlinkat(root, name, AT_REMOVEDIR) == 0 else { throw StatusFiles.error("清理队列") }
                        removedQueues.insert(name)
                    }
                } catch { clean = false }
            }
            do {
                if !removedRoot {
                    guard try StatusFiles.node(parent, name: name)?.sameNode(rootIdentity) == true else {
                        throw SimulatorStatusPublisherError.invalid("清理时会话目录被替换")
                    }
                    guard unlinkat(parent, name, AT_REMOVEDIR) == 0 else { throw StatusFiles.error("清理会话") }
                    removedRoot = true
                }
            } catch { clean = false }
            return clean
        }
    }
}

nonisolated private enum StatusFiles {
    struct Identity: Equatable {
        let device: dev_t, inode: ino_t, owner: uid_t, mode: mode_t, links: nlink_t, size: off_t
        let modifiedSeconds: Int
        let modifiedNanos: Int, changedSeconds: Int, changedNanos: Int
        init(_ value: stat) {
            device = value.st_dev; inode = value.st_ino; owner = value.st_uid; mode = value.st_mode
            links = value.st_nlink; size = value.st_size; modifiedSeconds = value.st_mtimespec.tv_sec
            modifiedNanos = value.st_mtimespec.tv_nsec; changedSeconds = value.st_ctimespec.tv_sec; changedNanos = value.st_ctimespec.tv_nsec
        }
        func sameNode(_ other: Self) -> Bool { device == other.device && inode == other.inode && owner == other.owner && mode == other.mode }
    }
    static func error(_ operation: String) -> SimulatorStatusPublisherError { .io(operation, errno) }
    static func info(_ fd: Int32) throws -> Identity {
        var value = stat()
        guard fstat(fd, &value) == 0 else { throw error("读取元信息") }
        return Identity(value)
    }
    static func node(_ parent: Int32, name: String) throws -> Identity? {
        guard !name.isEmpty, !name.contains("/"), !name.contains("\0"), name != ".", name != ".." else {
            throw SimulatorStatusPublisherError.invalid("文件名无效")
        }
        var value = stat()
        guard fstatat(parent, name, &value, AT_SYMLINK_NOFOLLOW) == 0 else {
            if errno == ENOENT { return nil }
            throw error("检查节点")
        }
        return Identity(value)
    }
    static func privateDirectory(_ value: Identity) throws {
        guard value.owner == getuid(), value.mode & S_IFMT == S_IFDIR, value.mode & 0o7777 == 0o700 else {
            throw SimulatorStatusPublisherError.invalid("目录必须是当前用户的 0700 私有目录")
        }
    }
    static func privateFile(_ value: Identity, maximum: Int) throws {
        guard value.owner == getuid(), value.mode & S_IFMT == S_IFREG, value.mode & 0o7777 == 0o600,
              value.links == 1, value.size >= 0, value.size <= maximum else {
            throw SimulatorStatusPublisherError.invalid("文件必须是当前用户的 0600 单链接有界常规文件")
        }
    }
    static func openChild(_ parent: Int32, name: String) throws -> Int32 {
        let fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw error("打开队列") }
        do { try privateDirectory(info(fd)); return fd } catch { Darwin.close(fd); throw error }
    }
    static func names(_ fd: Int32) throws -> [String] {
        // Reopen '.' to obtain an independent enumeration offset each tick.
        let copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard copy >= 0 else { throw error("打开目录枚举") }
        guard let directory = fdopendir(copy) else { Darwin.close(copy); throw error("枚举队列") }
        defer { closedir(directory) }
        var result: [String] = []
        errno = 0
        while let entry = readdir(directory) {
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) { String(cString: $0) }
            }
            if name == "." || name == ".." { continue }
            guard result.count < 64 else { throw SimulatorStatusPublisherError.invalid("队列文件数量超过限制") }
            result.append(name)
            errno = 0
        }
        guard errno == 0 else { throw error("读取队列目录") }
        return result
    }
    static func read(_ parent: Int32, name: String, maximum: Int) throws -> (Data, Identity) {
        let fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard fd >= 0 else { throw error("打开请求") }
        defer { Darwin.close(fd) }
        let before = try info(fd)
        try privateFile(before, maximum: maximum)
        var data = Data(count: Int(before.size))
        try data.withUnsafeMutableBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let count = Darwin.read(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw error("读取请求") }
                offset += count
            }
        }
        guard before == (try info(fd)), try node(parent, name: name) == before else {
            throw SimulatorStatusPublisherError.invalid("请求在读取期间被更改")
        }
        return (data, before)
    }
    static func publish(_ parent: Int32, name: String, object: [String: Any], replacing: Identity? = nil) throws -> Identity {
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        guard data.count <= 65_536 else { throw SimulatorStatusPublisherError.invalid("响应超过大小限制") }
        return try publishBytes(parent, name: name, data: data, replacing: replacing)
    }
    static func publishBytes(_ parent: Int32, name: String, data: Data, replacing: Identity? = nil) throws -> Identity {
        guard data.count <= 16 * 1024 * 1024 else { throw SimulatorStatusPublisherError.invalid("表盘超过大小限制") }
        let temporary = ".publish-" + UUID().uuidString.lowercased()
        let fd = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw error("创建发布文件") }
        defer { Darwin.close(fd) }
        let original = try info(fd)
        var complete = false, published = false
        defer {
            try? remove(parent, name: temporary, identity: original, matchingNodeOnly: true)
            if published, !complete { try? remove(parent, name: name, identity: original, matchingNodeOnly: true) }
        }
        try privateFile(original, maximum: 16 * 1024 * 1024)
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let count = Darwin.write(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw error("写入响应") }
                offset += count
            }
        }
        guard fsync(fd) == 0 else { throw error("同步响应") }
        if let replacing {
            guard try node(parent, name: name) == replacing else { throw SimulatorStatusPublisherError.invalid("租约文件被替换") }
            guard renameat(parent, temporary, parent, name) == 0 else { throw error("更新租约") }
        } else {
            guard renameatx_np(parent, temporary, parent, name, UInt32(RENAME_EXCL)) == 0 else { throw error("发布响应") }
        }
        published = true
        let result = try info(fd)
        guard result.sameNode(original), result.size == data.count, result.links == 1,
              try node(parent, name: name) == result else { throw SimulatorStatusPublisherError.invalid("发布文件被更改") }
        guard fsync(parent) == 0 else { throw error("同步目录") }
        complete = true
        return result
    }
    static func remove(_ parent: Int32, name: String, identity: Identity, matchingNodeOnly: Bool = false) throws {
        guard let current = try node(parent, name: name) else { return }
        guard current.links == 1, matchingNodeOnly ? current.sameNode(identity) : current == identity else { return }
        guard unlinkat(parent, name, 0) == 0 else {
            if errno == ENOENT { return }
            throw error("删除会话文件")
        }
    }
}

// Reuses the reviewed, self-authored management JSON parser; no fixture import.
/// Bounded parser rejects duplicate keys, invalid UTF-8, nonfinite numbers and
/// excessive nesting instead of accepting ambiguous authorization fields.
nonisolated private indirect enum StatusRequestJSON: Equatable, Sendable {
    case object([String: Self]), array([Self]), string(String), number(Double), bool(Bool), null
    var object: [String: Self]? { if case .object(let value) = self { value } else { nil } }
    var array: [Self]? { if case .array(let value) = self { value } else { nil } }
    var string: String? { if case .string(let value) = self { value } else { nil } }
    var bool: Bool? { if case .bool(let value) = self { value } else { nil } }
    var integer: Int? {
        if case .number(let value) = self, value.rounded() == value,
           value >= Double(Int32.min), value <= Double(Int32.max) { return Int(value) }
        return nil
    }
    subscript(_ key: String) -> Self? { object?[key] }

    static func decode(_ data: Data, maximum: Int = 65_536) throws -> Self {
        guard !data.isEmpty, data.count <= maximum, String(data: data, encoding: .utf8) != nil else {
            throw SimulatorStatusPublisherError.invalid("JSON 长度或编码无效")
        }
        var parser = Parser(bytes: Array(data))
        let result = try parser.value(depth: 0)
        parser.whitespace()
        guard parser.index == parser.bytes.count else { throw SimulatorStatusPublisherError.invalid("JSON 有额外内容") }
        return result
    }

    private struct Parser {
        let bytes: [UInt8]
        var index = 0
        var current: UInt8? { index < bytes.count ? bytes[index] : nil }
        mutating func whitespace() { while let byte = current, [9, 10, 13, 32].contains(byte) { index += 1 } }
        mutating func take(_ byte: UInt8) -> Bool {
            whitespace()
            guard current == byte else { return false }
            index += 1
            return true
        }
        func invalid() -> SimulatorStatusPublisherError { .invalid("JSON 结构无效或键重复") }
        mutating func text() throws -> String {
            whitespace()
            let start = index
            guard current == 34 else { throw invalid() }
            index += 1
            while let byte = current {
                index += 1
                if byte == 34 {
                    return try JSONDecoder().decode(String.self, from: Data(bytes[start..<index]))
                }
                if byte == 92 {
                    guard current != nil else { throw invalid() }
                    index += 1
                } else if byte < 32 { throw invalid() }
            }
            throw invalid()
        }
        mutating func value(depth: Int) throws -> StatusRequestJSON {
            guard depth <= 24 else { throw invalid() }
            whitespace()
            guard let byte = current else { throw invalid() }
            if byte == 123 {
                index += 1
                var result: [String: StatusRequestJSON] = [:]
                if take(125) { return .object(result) }
                repeat {
                    let key = try text()
                    guard result[key] == nil, take(58) else { throw invalid() }
                    result[key] = try value(depth: depth + 1)
                    if take(125) { return .object(result) }
                } while take(44)
                throw invalid()
            }
            if byte == 91 {
                index += 1
                var result: [StatusRequestJSON] = []
                if take(93) { return .array(result) }
                repeat {
                    result.append(try value(depth: depth + 1))
                    if take(93) { return .array(result) }
                } while take(44)
                throw invalid()
            }
            if byte == 34 { return .string(try text()) }
            let start = index
            while let next = current, ![9, 10, 13, 32, 44, 93, 125].contains(next) { index += 1 }
            let token = Data(bytes[start..<index])
            if token == Data("true".utf8) { return .bool(true) }
            if token == Data("false".utf8) { return .bool(false) }
            if token == Data("null".utf8) { return .null }
            guard let number = try? JSONDecoder().decode(Double.self, from: token), number.isFinite else { throw invalid() }
            return .number(number)
        }
    }
}
