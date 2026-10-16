import Foundation
import Darwin

/// A GDB server the user asked for on this launch. QEMU listens on loopback
/// only; port 0 lets it choose a free port, which the runner then reports.
nonisolated struct StandaloneDebugger: Codable, Equatable, Sendable {
    let port: Int
    let waitForAttach: Bool
    init(port: Int, waitForAttach: Bool) { self.port = port; self.waitForAttach = waitForAttach }
    var isValid: Bool { (0...65_535).contains(port) }
    /// Appended last to the fixed native guest invocation.
    var arguments: [String] { ["--gdb", String(port)] + (waitForAttach ? ["--gdb-wait"] : []) }
    /// Only a TCP listener on localhost; no outbound network is granted.
    var sandboxRule: String {
        "(allow network-bind network-inbound (local ip \"localhost:\(port == 0 ? "*" : String(port))\"))"
    }
}

nonisolated struct StandaloneBrokerConfiguration: Codable, Sendable {
    let version: Int
    let id: String
    let run: String
    let approvalDigest: String
    let maxRuntimeSeconds: Int
    let debugger: StandaloneDebugger?
    static let keys: Set<String> = ["version", "id", "run", "approvalDigest", "maxRuntimeSeconds"]
    static func validRun(_ value: String) -> Bool {
        value.hasPrefix("m-") && value.utf8.count == 14 && value.dropFirst(2).utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    static func prepare(session: URL, digest: String, seconds: Int, debugger: StandaloneDebugger? = nil) throws -> Self {
        guard FirmwareLibraryValidation.digest(digest), StandaloneRunLimits.isValid(seconds),
              debugger?.isValid ?? true else { throw SimulatorError.unsafeFile("原生代理批准无效") }
        _ = try StandaloneSandboxFiles.directory(session, privateOnly: true)
        let folder = session.appendingPathComponent("native-broker", isDirectory: true)
        let path = folder.appendingPathComponent("config.json")
        let value: Self
        if Darwin.access(folder.path, F_OK) == 0 {
            _ = try StandaloneSandboxFiles.directory(folder, privateOnly: true)
            let bytes = try StandaloneSandboxFiles.regular(path, limit: 4_096, privateOnly: true)
            guard Set(try StandaloneProcessSandbox.object(bytes).keys).subtracting(["debugger"]) == keys else { throw SimulatorError.unsafeFile("原生代理配置字段不匹配") }
            value = try JSONDecoder().decode(Self.self, from: bytes)
        } else {
            try StandaloneSandboxFiles.createPrivate(folder)
            value = Self(version: 1, id: UUID().uuidString.lowercased(),
                run: "m-" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(12),
                approvalDigest: digest, maxRuntimeSeconds: seconds, debugger: debugger)
            try StandaloneSandboxFiles.writeExclusive(try JSONEncoder().encode(value), to: path)
        }
        guard value.version == 1, UUID(uuidString: value.id)?.uuidString.lowercased() == value.id,
              validRun(value.run), value.approvalDigest == digest, value.maxRuntimeSeconds == seconds,
              value.debugger == debugger, value.debugger?.isValid ?? true else {
            throw SimulatorError.unsafeFile("原生代理配置与本次批准不一致")
        }
        let run = session.appendingPathComponent(value.run, isDirectory: true)
        if Darwin.access(run.path, F_OK) != 0 { try StandaloneSandboxFiles.createPrivate(run) }
        _ = try StandaloneSandboxFiles.directory(run, privateOnly: true)
        let temporary = run.appendingPathComponent("tmp", isDirectory: true)
        if Darwin.access(temporary.path, F_OK) != 0 { try StandaloneSandboxFiles.createPrivate(temporary) }
        _ = try StandaloneSandboxFiles.directory(temporary, privateOnly: true)
        return value
    }
    func validateLaunch(_ data: Data) throws {
        struct Request: Decodable { let version: Int; let id: String; let run: String }
        guard Set(try StandaloneProcessSandbox.object(data).keys) == ["version", "id", "run"] else { throw SimulatorError.unsafeFile("原生启动请求字段无效") }
        let request = try JSONDecoder().decode(Request.self, from: data)
        guard request.version == 1, request.id == id, request.run == run else { throw SimulatorError.unsafeFile("原生启动请求超出本次批准") }
    }
    func validateStop(_ data: Data) throws {
        struct Request: Decodable { let version: Int; let id: String }
        guard Set(try StandaloneProcessSandbox.object(data).keys) == ["version", "id"] else { throw SimulatorError.unsafeFile("原生停止请求字段无效") }
        let request = try JSONDecoder().decode(Request.self, from: data)
        guard request.version == 1, request.id == id else { throw SimulatorError.unsafeFile("原生停止请求不是当前会话") }
    }
}

/// Two independently installed OS policies. The app is the only launcher; H
/// requests a single fixed run and never supplies executable, argv, env or PID.
nonisolated struct StandaloneNativeLaunch: Sendable {
    let runtime: StandaloneBundledRuntime
    let session: URL
    let configuration: StandaloneBrokerConfiguration
    let helper: StandaloneSandboxLaunch
    let guest: StandaloneSandboxLaunch
    let configData: Data
    let configIdentity: StandaloneFileIdentity
    let directoryIdentity: StandaloneFileIdentity
    let sessionIdentity: StandaloneFileIdentity
    var brokerDirectory: URL { session.appendingPathComponent("native-broker", isDirectory: true) }
    var runDirectory: URL { session.appendingPathComponent(configuration.run, isDirectory: true) }
    static func prepare(runtime: StandaloneBundledRuntime, session: URL, digest: String, seconds: Int,
                        debugger: StandaloneDebugger? = nil) throws -> Self {
        let config = try StandaloneBrokerConfiguration.prepare(session: session, digest: digest, seconds: seconds, debugger: debugger)
        let folder = session.appendingPathComponent("native-broker", isDirectory: true)
        let configURL = folder.appendingPathComponent("config.json")
        let temporary = session.appendingPathComponent("host-tmp", isDirectory: true)
        if Darwin.access(temporary.path, F_OK) != 0 { try StandaloneSandboxFiles.createPrivate(temporary) }
        let helper = try StandaloneProcessSandbox.makeServe(runtime: runtime, session: session, approvedIdentity: digest,
            maxRuntime: seconds, library: BundledFirmwareService.libraryRoot, temporary: temporary)
        let guest = try StandaloneProcessSandbox.makeServe(runtime: runtime, session: session, approvedIdentity: digest,
            maxRuntime: seconds, library: BundledFirmwareService.libraryRoot, temporary: temporary,
            guestRun: session.appendingPathComponent(config.run), debugger: config.debugger)
        return Self(runtime: runtime, session: session, configuration: config, helper: helper, guest: guest,
            configData: try StandaloneSandboxFiles.regular(configURL, limit: 4_096, privateOnly: true),
            configIdentity: try StandaloneFileIdentity(configURL), directoryIdentity: try StandaloneFileIdentity(folder),
            sessionIdentity: try StandaloneFileIdentity(session))
    }
    func revalidateConfiguration() throws {
        guard try StandaloneFileIdentity(session) == sessionIdentity,
              try StandaloneFileIdentity(brokerDirectory) == directoryIdentity,
              try StandaloneFileIdentity(brokerDirectory.appendingPathComponent("config.json")) == configIdentity,
              try StandaloneSandboxFiles.regular(brokerDirectory.appendingPathComponent("config.json"), limit: 4_096, privateOnly: true) == configData else {
            throw SimulatorError.unsafeFile("原生代理配置在检查后改变")
        }
    }
}

nonisolated struct StandaloneFileIdentity: Equatable, Sendable {
    let device: dev_t
    let inode: ino_t
    init(_ url: URL) throws {
        let fd = Darwin.open(url.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("原生代理文件无法安全打开") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_uid == geteuid(),
              (info.st_mode & S_IFMT == S_IFDIR && info.st_mode & 0o777 == 0o700)
                || (info.st_mode & S_IFMT == S_IFREG && info.st_mode & 0o777 == 0o600 && info.st_nlink == 1) else {
            throw SimulatorError.unsafeFile("原生代理文件属性无效")
        }
        device = info.st_dev; inode = info.st_ino
    }
}

actor StandaloneGuestBroker {
    private let launch: StandaloneNativeLaunch
    private var helper: Process?
    private var guest: Process?
    private var guestOutput: FileHandle?
    private var guestInput: Pipe?
    private var ownedGroup: pid_t?
    private var monitor: Task<Void, Never>?
    private var stopping: Task<Bool, Never>?
    private var stopRequested = false
    private var launched = false
    private var stopConsumed = false
    private var stateIdentity: StandaloneFileIdentity?
    private var helperDeadline: ContinuousClock.Instant?
    private var launchRequestDeadline: ContinuousClock.Instant?
    private var guestDeadline: ContinuousClock.Instant?
    private var helperFinishDeadline: ContinuousClock.Instant?
    private var guestPID: pid_t?
    private var terminalPublished = false
    private(set) var failure: String?
    init(launch: StandaloneNativeLaunch) { self.launch = launch }

    func begin(helper: Process) throws {
        guard self.helper == nil, !stopRequested else { throw SimulatorError.transport("原生代理只能启动一次") }
        try launch.revalidateConfiguration()
        self.helper = helper
        // Unlimited sessions have no lifetime deadline. Startup and explicit
        // teardown retain independent deadlines even for an unlimited guest.
        helperDeadline = StandaloneRunLimits.deadline(seconds: launch.configuration.maxRuntimeSeconds, graceSeconds: 120)
        launchRequestDeadline = .now + .seconds(120)
        monitor = Task { await self.watch() }
    }
    private func watch() async {
        while !Task.isCancelled, !stopRequested {
            do {
                try launch.revalidateConfiguration()
                guard let helper, helper.isRunning,
                      helperDeadline.map({ ContinuousClock.now < $0 }) ?? true,
                      launched || (launchRequestDeadline.map({ ContinuousClock.now < $0 }) ?? false) else {
                    stopRequested = true
                    _ = await stopGuest()
                    if let helper = self.helper { _ = await StandaloneProcessReaper.stop(helper) }
                    return
                }
                let request = launch.brokerDirectory.appendingPathComponent("launch.json")
                if !launched, Darwin.access(request.path, F_OK) == 0 {
                    try launch.configuration.validateLaunch(StandaloneSandboxFiles.regular(request, limit: 4_096, privateOnly: true))
                    launched = true // Consume before validation I/O; never retry or extend budget.
                    launchRequestDeadline = nil
                    try await startGuest()
                }
                let stop = launch.brokerDirectory.appendingPathComponent("stop.json")
                if !stopConsumed, Darwin.access(stop.path, F_OK) == 0 {
                    try launch.configuration.validateStop(StandaloneSandboxFiles.regular(stop, limit: 4_096, privateOnly: true))
                    stopConsumed = true
                    _ = await stopGuest()
                    helperFinishDeadline = .now + .seconds(2)
                }
                if let guest {
                    if !guest.isRunning || (guestDeadline.map { ContinuousClock.now >= $0 } ?? false) {
                        _ = await stopGuest()
                        if helperFinishDeadline == nil { helperFinishDeadline = .now + .seconds(2) }
                    }
                }
                // Keep the deadline watcher after guest exit. Briefly retain H
                // so its pending status/stop response can observe terminal state.
                if let helperFinishDeadline, ContinuousClock.now >= helperFinishDeadline {
                    stopRequested = true
                    _ = await StandaloneProcessReaper.stop(helper)
                    return
                }
                try await Task.sleep(for: .milliseconds(50))
            } catch {
                failure = error.localizedDescription
                stopRequested = true
                let reaped = await stopGuest()
                try? publish(state: "failed", pid: guestPID, exitCode: -1, cleanup: reaped ? "reaped" : "unconfirmed", error: "原生代理拒绝无效或已改变的请求")
                if let helper { _ = await StandaloneProcessReaper.stop(helper) }
                return
            }
        }
    }

    private func startGuest() async throws {
        guard !stopRequested, guest == nil else { throw CancellationError() }
        let prepared = launch
        let fresh = try await Task.detached(priority: .userInitiated) {
            // H cannot change prepared inputs under its policy. This also catches
            // modifications by another same-user process between approval and run.
            let runtime = try StandaloneBundledRuntime.resolve(resources: prepared.runtime.engine.deletingLastPathComponent())
            return try StandaloneProcessSandbox.makeServe(runtime: runtime, session: prepared.session,
                approvedIdentity: prepared.configuration.approvalDigest, maxRuntime: prepared.configuration.maxRuntimeSeconds,
                library: BundledFirmwareService.libraryRoot, temporary: prepared.helper.temporaryDirectory,
                guestRun: prepared.runDirectory, debugger: prepared.configuration.debugger)
        }.value
        guard !stopRequested, !Task.isCancelled, fresh.policySHA256 == launch.guest.policySHA256,
              fresh.arguments == launch.guest.arguments else { throw CancellationError() }
        try launch.revalidateConfiguration()
        let log = launch.runDirectory.appendingPathComponent("adapter-runner.log")
        let fd = Darwin.open(log.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW_ANY | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("无法独占创建客体日志") }
        let output = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        let input = Pipe(), process = Process()
        try launch.guest.configure(process)
        process.standardInput = input; process.standardOutput = output; process.standardError = output
        try process.run()
        guest = process; guestPID = process.processIdentifier; guestOutput = output; guestInput = input
        guestDeadline = StandaloneRunLimits.deadline(seconds: launch.configuration.maxRuntimeSeconds)
        // G installs handlers and forms its own group before waiting on this
        // anonymous pipe. H has no path or descriptor with which to send GO.
        let end = ContinuousClock.now + .seconds(1)
        while process.isRunning, getpgid(process.processIdentifier) != process.processIdentifier,
              ContinuousClock.now < end, !stopRequested {
            try await Task.sleep(for: .milliseconds(10))
        }
        guard !stopRequested, process.isRunning, getpgid(process.processIdentifier) == process.processIdentifier else {
            throw SimulatorError.transport("无法核实本应用创建的客体进程组")
        }
        ownedGroup = process.processIdentifier
        try input.fileHandleForWriting.write(contentsOf: Data("AZKI-GUEST-GO\n".utf8))
        try input.fileHandleForWriting.close()
        try publish(state: "running", pid: process.processIdentifier)
    }

    @discardableResult func stop() async -> Bool {
        stopRequested = true
        monitor?.cancel(); monitor = nil
        return await stopGuest()
    }
    private func stopGuest() async -> Bool {
        if let stopping { return await stopping.value }
        guard let process = guest else { return true }
        let group = ownedGroup
        let task = Task { [self] in
            try? guestInput?.fileHandleForWriting.close()
            // The supervisor gives its runner eight seconds to close input,
            // collect guest health and publish its joint storage receipt.
            let reaped = await StandaloneProcessReaper.stop(process, confirmedGroup: group, graceSeconds: 10)
            let code = process.isRunning ? -1 : (process.terminationReason == .uncaughtSignal ? -process.terminationStatus : process.terminationStatus)
            try? publish(state: reaped ? "exited" : "failed", pid: process.processIdentifier,
                         exitCode: code, cleanup: reaped ? "reaped" : "unconfirmed")
            if reaped {
                try? guestOutput?.close(); guestOutput = nil; guestInput = nil
                launch.guest.cleanup(); guest = nil; ownedGroup = nil
            } else { failure = "客体进程组回收尚未确认" }
            stopping = nil
            return reaped
        }
        stopping = task
        return await task.value
    }
    private func publish(state: String, pid: pid_t? = nil, exitCode: Int32? = nil, cleanup: String? = nil, error: String? = nil) throws {
        // The bridge treats a terminal record as immutable. A later cleanup
        // retry may improve our own verdict but must not rewrite its history.
        guard !terminalPublished else { return }
        try launch.revalidateConfiguration()
        var value: [String: Any] = ["version": 1, "id": launch.configuration.id, "run": launch.configuration.run, "state": state]
        if let pid { value["pid"] = pid }
        if let exitCode { value["exitCode"] = exitCode }
        if let cleanup { value["cleanup"] = cleanup }
        if let error { value["error"] = error }
        let final = launch.brokerDirectory.appendingPathComponent("state.json")
        if let stateIdentity { guard try StandaloneFileIdentity(final) == stateIdentity else { throw SimulatorError.unsafeFile("代理状态文件被替换") } }
        let temporary = launch.brokerDirectory.appendingPathComponent(".state-" + UUID().uuidString.lowercased() + ".tmp")
        try StandaloneSandboxFiles.writeExclusive(try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]), to: temporary)
        let identity = try StandaloneFileIdentity(temporary)
        guard renamex_np(temporary.path, final.path, stateIdentity == nil ? UInt32(RENAME_EXCL) : 0) == 0 else {
            throw SimulatorError.unsafeFile("代理状态原子发布失败")
        }
        stateIdentity = identity
        if state != "running" { terminalPublished = true }
    }
}
