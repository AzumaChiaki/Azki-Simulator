import Foundation
import Observation
import Darwin

/// The app manages its own helper only. Opening another application's session
/// grants a temporary connection; it never transfers process ownership.
@MainActor @Observable
final class StandaloneRuntimeModel {
    var qemuURL: URL? { didSet { selectionChanged() } }
    var firmwareURL: URL? { didSet { selectionChanged() } }
    var resourceURL: URL? { didSet { selectionChanged() } }
    private(set) var sessionURL: URL?
    private var operationCount = 0
    private(set) var isStopping = false
    var isBusy: Bool { operationCount > 0 || isStopping }
    private(set) var isServing = false
    private(set) var statusMessage = "选择内置固件，或导入自己的固件"
    var errorMessage: String?
    private(set) var selectedSummary = "尚未准备运行文件"
    let runtimeSession = SimulatorSessionModel()
    let connectionSettings = ConnectionSettings.shared
    var skipOOBE = true {
        didSet {
            if oldValue != skipOOBE && !isServing {
                revision &+= 1
                prepared = nil; bundledPrepared = nil
            }
        }
    }
    var virtualIdentity: SimulatorVirtualIdentity? {
        didSet {
            if oldValue != virtualIdentity && !isServing {
                revision &+= 1
                prepared = nil; bundledPrepared = nil
            }
        }
    }
    private(set) var firmwareItems: [BundledFirmwareItem] = []
    private(set) var libraryStatus = "正在读取内置固件目录"
    private(set) var libraryAvailable = false
    private(set) var libraryRuntimeReady = false
    private(set) var hasLoadedCatalog = false
    private(set) var importCandidateIDs: [String] = []
    private(set) var importFailureMessage: String?
    private(set) var importDiagnostic: FirmwareDiagnostic?
    private(set) var importDiagnosticError: String?
    let modelConstraint: String?
    let instanceID: UUID?
    private(set) var instanceStorageMessage = "用户数据盘尚未启用"
    @ObservationIgnored private var storageSession: URL?
    var selectedProfileID: String? { didSet {
        guard oldValue != selectedProfileID else { return }
        revision &+= 1
        bundledPrepared = nil
        prepared = nil
        selectedSummary = "尚未准备所选固件"
    } }
    @ObservationIgnored private var prepared: StandalonePreparedSession?
    @ObservationIgnored private var bundledPrepared: BundledPreparedSession?
    @ObservationIgnored private let firmwareService: BundledFirmwareService
    @ObservationIgnored private let maximumRuntimeSeconds: Int
    @ObservationIgnored private var revision: UInt64 = 0
    @ObservationIgnored private var helper: Process?
    @ObservationIgnored private var startingHelper: Process?
    @ObservationIgnored private var helperOutput: FileHandle?
    @ObservationIgnored private var helperLaunch: StandaloneSandboxLaunch?
    @ObservationIgnored private var guestBroker: StandaloneGuestBroker?
    @ObservationIgnored private var stopping: Task<Bool, Never>?

    // The optional owned process permits deterministic authored lifecycle tests.
    // Normal application construction supplies neither argument.
    init(ownedHelper: Process? = nil, helperOutput: FileHandle? = nil, helperIsStarting: Bool = false,
         firmwareService: BundledFirmwareService = BundledFirmwareService(),
         maximumRuntimeSeconds: Int = StandaloneRunLimits.unlimited,
         modelConstraint: String? = nil, instanceID: UUID? = nil) {
        precondition(StandaloneRunLimits.isValid(maximumRuntimeSeconds))
        self.maximumRuntimeSeconds = maximumRuntimeSeconds
        self.modelConstraint = modelConstraint
        self.instanceID = instanceID
        self.helper = ownedHelper
        self.startingHelper = helperIsStarting ? ownedHelper : nil
        self.helperOutput = helperOutput
        self.isServing = ownedHelper != nil
        self.firmwareService = firmwareService
        if let ownedHelper { observeTermination(of: ownedHelper) }
    }

    var canStartPrepared: Bool { (prepared != nil || bundledPrepared != nil) && !isBusy && !isServing && !runtimeSession.isAttached }
    var canPrepare: Bool { qemuURL != nil && firmwareURL != nil && resourceURL != nil && !isBusy && !isServing && !runtimeSession.isAttached }
    var preparedRunLimitSeconds: Int {
        StandaloneRunLimits.effective(bundledPrepared?.runLimitSeconds ?? StandaloneRunLimits.unlimited, maximumRuntimeSeconds)
    }
    var preparedExecutionApproved: Bool { bundledPrepared?.executionApproved == true }
    var selectedFirmware: BundledFirmwareItem? { firmwareItems.first { $0.id == selectedProfileID } }
    var selectableFirmwareItems: [BundledFirmwareItem] {
        firmwareItems.filter { modelConstraint == nil || $0.model == modelConstraint }
    }
    var canRunSelectedFirmware: Bool {
        libraryRuntimeReady && selectedFirmware?.availability.ready == true
            && (modelConstraint == nil || selectedFirmware?.model == modelConstraint)
            && !isBusy && !isServing && !runtimeSession.isAttached
    }

    func loadCatalog() async {
        guard !isBusy else { return }
        let token = revision
        operationCount += 1
        defer { operationCount -= 1; if token == revision { hasLoadedCatalog = true } }
        do {
            let catalog = try BundledFirmwareCatalog.decode(await firmwareService.run(["library-catalog"]))
            guard token == revision, !Task.isCancelled, !isStopping else { return }
            applyCatalog(catalog)
        }
        catch {
            guard token == revision, !Task.isCancelled else { return }
            firmwareItems = []
            libraryAvailable = false
            libraryRuntimeReady = false
            libraryStatus = error.localizedDescription
        }
    }

    private func applyCatalog(_ catalog: BundledFirmwareCatalog) {
        firmwareItems = catalog.items
        libraryAvailable = true
        libraryRuntimeReady = catalog.runtime.ready
        libraryStatus = catalog.runtime.reason.isEmpty ? "固件目录已就绪" : catalog.runtime.reason
        if let selectedProfileID, !selectableFirmwareItems.contains(where: { $0.id == selectedProfileID }) {
            self.selectedProfileID = nil
        }
    }

    func importFirmware(_ url: URL) async {
        guard !isBusy, !isServing, !runtimeSession.isAttached else { return }
        let token = revision
        let granted = url.startAccessingSecurityScopedResource()
        defer { if granted { url.stopAccessingSecurityScopedResource() } }
        operationCount += 1
        errorMessage = nil
        importFailureMessage = nil
        importDiagnostic = nil
        importDiagnosticError = nil
        defer { operationCount -= 1 }
        let receipt: FirmwareImportReceipt
        do {
            receipt = try FirmwareImportReceipt.decode(await firmwareService.run(["library-import", "--input", url.path]))
        } catch {
            guard token == revision, !Task.isCancelled, !isStopping else { return }
            let importFailure = error.localizedDescription
            do {
                let diagnostic = try FirmwareDiagnostic.decode(await firmwareService.run(["library-probe", "--input", url.path]))
                guard token == revision, !Task.isCancelled, !isStopping else { return }
                importFailureMessage = importFailure
                importDiagnostic = diagnostic
                importCandidateIDs = []
                selectedProfileID = nil
                statusMessage = diagnostic.summary
                // An unknown identity is expected during development. Keep its
                // readable diagnostic inline, without a second modal or a run.
                errorMessage = nil
            } catch {
                guard token == revision, !Task.isCancelled, !isStopping else { return }
                importFailureMessage = importFailure
                importDiagnosticError = String(error.localizedDescription.prefix(1_024))
                errorMessage = importFailure
            }
            return
        }
        guard token == revision, !Task.isCancelled, !isStopping else { return }
        do {
            let catalog = try BundledFirmwareCatalog.decode(await firmwareService.run(["library-catalog"]))
            guard token == revision, !Task.isCancelled, !isStopping else { return }
            applyCatalog(catalog)
            importCandidateIDs = receipt.candidateIDs
            let candidates = receipt.candidateIDs.filter { id in selectableFirmwareItems.contains { $0.id == id } }
            selectedProfileID = candidates.count == 1 ? candidates.first : nil
            if candidates.isEmpty {
                statusMessage = "固件已导入，但不属于本窗口的型号；请在对应型号窗口选择。"
            } else {
                statusMessage = candidates.count > 1
                    ? "固件已导入；相同文件对应多个型号，请明确选择目标型号。"
                    : "固件已导入，将按本实例的硬件预设启动；不兼容时会提示原因。"
            }
        } catch {
            guard token == revision, !Task.isCancelled else { return }
            errorMessage = error.localizedDescription
            importFailureMessage = error.localizedDescription
        }
    }

    /// Prepare happens internally after the user's Run action. It never starts
    /// a helper or executes firmware; the fresh receipt is the approval boundary.
    @discardableResult func prepareSelectedFirmware() async -> Bool {
        guard canRunSelectedFirmware, let profileID = selectedProfileID else { return false }
        operationCount += 1
        errorMessage = nil
        let token = revision
        defer { operationCount -= 1 }
        do {
            guard await finishInstanceStorage() else {
                errorMessage = instanceStorageMessage
                return false
            }
            let root = URL(fileURLWithPath: "/private/tmp", isDirectory: true)
                .appendingPathComponent("azki-b-" + UUID().uuidString.lowercased().prefix(12), isDirectory: true)
            var arguments = ["prepare-bundled", "--profile-id", profileID, "--session-root", root.path]
            if let instanceID { arguments += ["--instance-id", instanceID.uuidString.lowercased()] }
            if let virtualIdentity, instanceID != nil {
                arguments += ["--virtual-serial", virtualIdentity.serial, "--virtual-address", virtualIdentity.address]
            }
            arguments += [skipOOBE ? "--skip-oobe" : "--no-skip-oobe"]
            let output = try await firmwareService.run(arguments)
            let receipt = try BundledPreparedSession.decode(output, requestedRoot: root, profileID: profileID)
            guard token == revision, profileID == selectedProfileID else { return false }
            prepared = nil
            bundledPrepared = receipt
            sessionURL = receipt.root
            if receipt.instanceStorage?["available"]?.bool == true {
                storageSession = receipt.root
                instanceStorageMessage = receipt.instanceStorage?["norPersistent"]?.bool == true
                    ? "独立数据盘和设备配置已启用；版本切换保留上一快照。"
                    : "独立 NAND 数据盘已启用；NOR 配置尚未持久化。"
            } else {
                instanceStorageMessage = receipt.instanceStorage?["reason"]?.string ?? "此核心未提供用户数据写回；重启恢复初始数据。"
            }
            selectedSummary = receipt.summary + "\n\n完整身份 SHA-256：" + receipt.identityDigest
            statusMessage = "已核验所选固件；等待本次运行确认"
            return true
        } catch { errorMessage = error.localizedDescription; return false }
    }

    private func selectionChanged() {
        revision &+= 1
        prepared = nil
        bundledPrepared = nil
        selectedSummary = [("核心", qemuURL), ("AP 固件", firmwareURL), ("资源", resourceURL)]
            .map { "\($0.0)：\($0.1?.path ?? "未选择")" }.joined(separator: "\n")
        if !isServing { statusMessage = "文件选择已更新，请重新检查" }
    }

    func savedVirtualIdentity() async throws -> SimulatorVirtualIdentity? {
        guard let instanceID else { return nil }
        let data = try await firmwareService.run(["instance-identity", "--instance-id", instanceID.uuidString.lowercased()])
        struct Receipt: Decodable { let identity: SimulatorVirtualIdentity? }
        return try JSONDecoder().decode(Receipt.self, from: data).identity
    }

    func prepare() async {
        guard canPrepare, let qemuURL, let firmwareURL, let resourceURL else { return }
        let inputs = [qemuURL, firmwareURL, resourceURL]
        let grants = inputs.map { $0.startAccessingSecurityScopedResource() }
        defer { for (url, granted) in zip(inputs, grants) where granted { url.stopAccessingSecurityScopedResource() } }
        let token = revision
        operationCount += 1
        errorMessage = nil
        defer { operationCount -= 1 }
        do {
            let root = URL(fileURLWithPath: "/private/tmp", isDirectory: true)
                .appendingPathComponent("azki-sim-" + UUID().uuidString.lowercased().prefix(12), isDirectory: true)
            let output = try await firmwareService.run(["prepare", root.path,
                "--qemu", qemuURL.path, "--firmware", firmwareURL.path, "--resource", resourceURL.path])
            let result = try StandalonePreparedSession.decode(output, root: root)
            guard token == revision else { return }
            bundledPrepared = nil
            prepared = result
            sessionURL = root
            selectedSummary = result.summary
            statusMessage = "已完成静态检查；尚未执行核心或固件"
        } catch { errorMessage = error.localizedDescription }
    }

    /// Start the prepared instance using its private paths and sandbox policy.
    /// Executable contents are not rehashed as a second launch gate.
    func startApproved(maxRuntimeSeconds: Int? = nil) async {
        guard canStartPrepared else { return }
        let preparedRoot: URL
        guard maxRuntimeSeconds.map(StandaloneRunLimits.isValid) ?? true else {
            errorMessage = "运行时限超出已准备会话的范围"
            return
        }
        let runLimit = StandaloneRunLimits.effective(preparedRunLimitSeconds, maxRuntimeSeconds ?? StandaloneRunLimits.unlimited)
        let identity: String
        if let bundledPrepared {
            preparedRoot = bundledPrepared.root
            identity = bundledPrepared.identityDigest
        } else {
            errorMessage = "高级三文件会话尚未提供完整外层沙盒计划，仅支持静态检查；不会启动核心。"
            return
        }
        operationCount += 1
        errorMessage = nil
        let token = revision
        var launchedProcess: Process?
        defer { operationCount -= 1 }
        do {
            // H (session helper) and G (guest core) run under separate OS policies;
            // only this app launches G, through the native broker below.
            let native = try await Task.detached(priority: .userInitiated) {
                let runtime = try StandaloneBundledRuntime.resolve()
                return try StandaloneNativeLaunch.prepare(runtime: runtime, session: preparedRoot,
                    digest: identity, seconds: runLimit)
            }.value
            let launch = native.helper
            try Task.checkCancellation()
            guard token == revision, !isStopping else { throw CancellationError() }
            let outputPath = preparedRoot.appendingPathComponent("helper-" + UUID().uuidString.lowercased() + ".log").path
            let fd = Darwin.open(outputPath, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW_ANY | O_CLOEXEC, 0o600)
            guard fd >= 0 else { throw SimulatorError.unsafeFile("无法创建私有 helper 日志") }
            let output = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
            let process = Process()
            try launch.configure(process)
            process.standardInput = FileHandle.nullDevice
            process.standardOutput = output
            process.standardError = output
            observeTermination(of: process)
            try process.run()
            let broker = StandaloneGuestBroker(launch: native)
            // Record ownership synchronously after run(), before any actor hop.
            // Cancellation or a failed broker handshake must still reap H.
            guestBroker = broker
            helper = process
            startingHelper = process
            launchedProcess = process
            helperOutput = output
            helperLaunch = launch
            isServing = true
            sessionURL = preparedRoot
            try await broker.begin(helper: process)
            func checkCurrentAttempt() throws {
                try Task.checkCancellation()
                guard token == revision, !isStopping, helper === process, process.isRunning else { throw CancellationError() }
            }
            try await withTaskCancellationHandler(operation: {
                let deadline = ContinuousClock.now + .seconds(connectionSettings.timeoutSeconds)
                while !FileManager.default.fileExists(atPath: preparedRoot.appendingPathComponent("control.sock").path) {
                    try checkCurrentAttempt()
                    guard ContinuousClock.now < deadline else { throw SimulatorConnectionTimeout() }
                    try await Task.sleep(for: .milliseconds(25))
                }
                try checkCurrentAttempt()
                let connection = try await Task.detached { try SimulatorSessionConnection(directoryURL: preparedRoot) }.value
                do {
                    try checkCurrentAttempt()
                    _ = try await connection.command("start")
                    try checkCurrentAttempt()
                    await connection.close()
                    try checkCurrentAttempt()
                } catch {
                    await connection.close()
                    throw error
                }
                startingHelper = nil
                runtimeSession.attach(preparedRoot)
                statusMessage = runLimit == StandaloneRunLimits.unlimited
                    ? "独立核心已启动，可随时停止"
                    : "独立核心已启动，本次最长 \(StandaloneRunLimits.describe(runLimit))，随时可停止"
            }, onCancel: { [weak self] in
                Task { @MainActor [weak self] in
                    guard let self, self.helper === process else { return }
                    self.revision &+= 1
                    _ = await self.stopOwnedHelper(expected: process)
                }
            })
        } catch {
            if error is SimulatorConnectionTimeout, let process = launchedProcess,
               process.isRunning, helper === process, token == revision, !isStopping {
                // The start request may already have launched the guest. A
                // missing reply is not authority to tear that instance down.
                startingHelper = nil
                runtimeSession.attach(preparedRoot)
                errorMessage = error.localizedDescription
                statusMessage = "连接超时；实例保留，正在重新连接"
                return
            }
            // The termination callback and this catch may arrive in either
            // order. Capture our owned log before teardown closes its descriptor.
            let message = launchedProcess.flatMap { process in
                !process.isRunning ? Self.helperFailureMessage(process, output: helperOutput,
                    duringStartup: true) : nil
            } ?? error.localizedDescription
            let stopped: Bool
            if let launchedProcess { stopped = await stopOwnedHelper(expected: launchedProcess) }
            else { stopped = true }
            if stopped { await finishInstanceStorage() }
            // A cancelled/older attempt must not overwrite a later session's UI.
            guard token == revision else { return }
            errorMessage = message
            statusMessage = stopped ? "启动失败，核心已停止；原文件保持不变" : "启动失败；停止仍未确认，请重试停止"
        }
    }

    func attachExisting(_ url: URL) async {
        guard !isBusy else { return }
        operationCount += 1
        defer { operationCount -= 1 }
        guard await stopOwnedHelper() else {
            errorMessage = "旧会话停止尚未确认，未连接新会话"
            return
        }
        sessionURL = url
        runtimeSession.attach(url)
        statusMessage = "连接已有本机会话；关闭应用只断开该连接"
    }

    @discardableResult func stop() async -> Bool {
        operationCount += 1
        defer { operationCount -= 1 }
        revision &+= 1
        await firmwareService.cancelAll()
        let stopped = await stopOwnedHelper()
        if stopped { await finishInstanceStorage() }
        statusMessage = stopped ? "已断开；本应用创建的核心和 helper 已结束" : "停止尚未确认，请重试停止"
        if !stopped { errorMessage = "本应用的客体进程组或会话 helper 尚未确认退出" }
        return stopped
    }

    private func observeTermination(of process: Process) {
        process.terminationHandler = { [weak self] completed in
            Task { @MainActor [weak self] in
                guard let self, self.helper === completed, !self.isStopping else { return }
                let failure = Self.helperFailureMessage(completed, output: self.helperOutput,
                    duringStartup: self.startingHelper === completed)
                self.revision &+= 1
                let token = self.revision
                let stopped = await self.stopOwnedHelper(expected: completed)
                if stopped { await self.finishInstanceStorage() }
                guard token == self.revision else { return }
                self.statusMessage = stopped
                    ? (failure == nil ? "会话已结束，本应用创建的进程已回收" : "会话 helper 意外退出，本应用创建的进程已回收")
                    : "会话已结束，但客体回收尚未确认；请重试停止"
                if let failure { self.errorMessage = failure + (stopped ? "" : "\n客体进程组回收尚未确认") }
                else if !stopped { self.errorMessage = "客体进程组回收尚未确认" }
            }
        }
    }

    private static func helperFailureMessage(_ process: Process, output: FileHandle?, duringStartup: Bool) -> String? {
        guard !process.isRunning,
              duringStartup || process.terminationReason == .uncaughtSignal || process.terminationStatus != 0 else { return nil }
        let reason = process.terminationReason == .uncaughtSignal
            ? "信号 \(process.terminationStatus)" : "退出码 \(process.terminationStatus)"
        let message = (duringStartup ? "会话 helper 在启动完成前退出" : "会话 helper 意外退出") + "（\(reason)）。"
        // Read only the descriptor we exclusively created for this helper.
        // Never reopen a path from the helper or scan an unbounded log/pipe.
        guard let output else { return message }
        let fd = output.fileDescriptor
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG,
              info.st_uid == geteuid(), info.st_mode & 0o777 == 0o600,
              info.st_nlink == 1, info.st_size > 0 else { return message }
        var bytes = [UInt8](repeating: 0, count: Int(min(info.st_size, 4_096)))
        let offset = max(0, info.st_size - off_t(bytes.count))
        let count = bytes.withUnsafeMutableBytes { Darwin.pread(fd, $0.baseAddress, $0.count, offset) }
        guard count > 0 else { return message }
        let scalars = String(decoding: bytes.prefix(count), as: UTF8.self).unicodeScalars.filter {
            $0.value >= 32 || $0 == "\n" || $0 == "\t"
        }
        let detail = String(String.UnicodeScalarView(scalars)).trimmingCharacters(in: .whitespacesAndNewlines)
        return message + (detail.isEmpty ? "" : "\n" + String(detail.suffix(1_024)))
    }

    @discardableResult private func finishInstanceStorage() async -> Bool {
        guard let directory = storageSession, let instanceID else { return true }
        storageSession = nil
        operationCount += 1
        defer { operationCount -= 1 }
        do {
            let data = try await firmwareService.run(["instance-storage-finish", "--session", directory.path,
                "--instance-id", instanceID.uuidString.lowercased()])
            let result = try JSONDecoder().decode(SimulatorJSON.self, from: data)
            instanceStorageMessage = result["committed"]?.bool == true
                ? (result["norPersistent"]?.bool == true
                    ? "本次数据和设备配置已保存。" : "本次 NAND 数据已保存；NOR 配置尚未持久化。")
                : "本次数据未提交，保留上一快照。\(result["reason"]?.string ?? "请查看会话诊断。")"
            return true
        } catch {
            // A completed process is not proof that its snapshot receipt was
            // published. Keep the binding so Stop/Run can retry idempotently.
            if storageSession == nil { storageSession = directory }
            instanceStorageMessage = "数据盘提交未完成，已保留已有快照：\(error.localizedDescription)"
            return false
        }
    }

    private func stopOwnedHelper(expected: Process? = nil) async -> Bool {
        if let expected, helper !== expected { return true }
        runtimeSession.detach()
        if let stopping { return await stopping.value }
        let process = helper
        guard process != nil || guestBroker != nil else { return true }
        // Keep ownership and busy state until every concurrent caller observes
        // the same completed teardown. Capture this helper's output, never a
        // mutable handle that a later launch could replace.
        let output = helperOutput
        isStopping = true
        let broker = guestBroker
        let task = Task { @MainActor [self] in
            // Stop and reap the guest core first; then the helper.
            let guestStopped = await broker?.stop() ?? true
            let helperStopped: Bool
            if let process { helperStopped = await StandaloneProcessReaper.stop(process) }
            else { helperStopped = true }
            let stopped = guestStopped && helperStopped
            if stopped, helper === process {
                try? output?.close()
                helperOutput = nil
                helper = nil
                startingHelper = nil
                helperLaunch?.cleanup()
                helperLaunch = nil
                guestBroker = nil
                isServing = false
            }
            isStopping = false
            stopping = nil
            return stopped
        }
        stopping = task
        return await task.value
    }

}

nonisolated struct StandalonePreparedSession: Sendable {
    let root: URL
    let hashes: [String: String]
    let summary: String

    static func decode(_ data: Data, root: URL) throws -> Self {
        guard let value = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              value["version"] as? Int == 1, value["directory"] as? String == root.path,
              let core = value["core"] as? [String: [String: Any]] else {
            throw SimulatorError.invalidSession("静态检查没有返回完整输入清单")
        }
        var hashes: [String: String] = [:]
        var lines: [String] = []
        for (role, label) in [("qemu", "核心"), ("firmware", "AP 固件"), ("resource", "资源镜像")] {
            guard let item = core[role], let path = item["path"] as? String,
                  let sha = item["sha256"] as? String, sha.count == 64,
                  sha.allSatisfy({ $0.isASCII && ("0123456789abcdef".contains($0)) }) else {
                throw SimulatorError.invalidSession("输入指纹不完整")
            }
            hashes[role] = sha
            lines.append("\(label)：\(path)\nSHA-256：\(sha)\n\(item["classification"] as? String ?? "来源未验证")")
        }
        return Self(root: root, hashes: hashes, summary: lines.joined(separator: "\n\n"))
    }
}

nonisolated enum StandalonePython {
    /// Only runs this app's own static inspection command, never a selected
    /// executable. Input paths are argv values; no shell interpretation occurs.
    static func run(launch: StandaloneSandboxLaunch, command: String, owner: StandaloneOwnedCommand) throws -> Data {
        let process = Process()
        let pipe = Pipe()
        try launch.configure(process)
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = pipe
        process.standardError = pipe
        try owner.start(process)
        defer {
            if process.isRunning { owner.cancel(); process.waitUntilExit() }
        }
        // Importing an arbitrary full OTA hashes and validates every member.
        let timeout = command == "library-import" ? 300.0
            : ["prepare", "prepare-bundled", "library-probe"].contains(command) ? 45.0 : 15.0
        let deadline = StandaloneCommandDeadline()
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout) {
            if process.isRunning {
                deadline.markExpired()
                process.terminate()
                DispatchQueue.global().asyncAfter(deadline: .now() + 1) {
                    if process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
                }
            }
        }
        defer { try? pipe.fileHandleForReading.close() }
        var output = Data()
        while let part = try pipe.fileHandleForReading.read(upToCount: 4_096), !part.isEmpty {
            output.append(part)
            if output.count > FirmwareLibraryValidation.maximumResponseBytes {
                if process.isRunning {
                    process.terminate()
                    DispatchQueue.global().asyncAfter(deadline: .now() + 1) {
                        if process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
                    }
                }
                process.waitUntilExit()
                throw SimulatorError.transport("检查输出超过限制")
            }
        }
        process.waitUntilExit()
        try owner.checkCancellation()
        guard !deadline.expired else { throw SimulatorError.transport("固件检查超时，已结束本次检查进程") }
        guard process.terminationStatus == 0 else {
            throw SimulatorError.transport(failureMessage(output, exitStatus: process.terminationStatus))
        }
        return output
    }

    static func failureMessage(_ output: Data, exitStatus: Int32? = nil) -> String {
        struct Failure: Decodable {
            struct Detail: Decodable { let message: String }
            let ok: Bool
            let error: Detail
        }
        if output.count <= 65_536,
           let failure = try? JSONDecoder().decode(Failure.self, from: output), !failure.ok {
            let message = failure.error.message.trimmingCharacters(in: .whitespacesAndNewlines)
            if !message.isEmpty, message.utf8.count <= 1_024, !message.contains("\0") { return message }
        }
        // Keep launch/runtime failures visible too: dyld and Python startup
        // errors do not use our JSON envelope. Treat stderr only as text and
        // bound it, so a failed catalog never becomes an unexplained empty UI.
        let suffix = Data(output.suffix(2_048))
        let detail = String(decoding: suffix, as: UTF8.self).unicodeScalars.filter {
            $0.value >= 32 || $0 == "\n" || $0 == "\t"
        }
        let diagnostic = String(String.UnicodeScalarView(detail)).trimmingCharacters(in: .whitespacesAndNewlines)
        let status = exitStatus.map { "（退出码 \($0)）" } ?? ""
        return "固件检查失败\(status)；所选固件尚未运行。"
            + (diagnostic.isEmpty ? "" : "\n" + String(diagnostic.suffix(1_024)))
    }
}

private final class StandaloneCommandDeadline: @unchecked Sendable {
    private let lock = NSLock()
    private var didExpire = false
    var expired: Bool { lock.lock(); defer { lock.unlock() }; return didExpire }
    func markExpired() { lock.lock(); didExpire = true; lock.unlock() }
}
