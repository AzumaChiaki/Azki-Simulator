import Foundation
import Darwin

@MainActor final class ESP32BridgeSession {
    private var process: Process?
    private var output: FileHandle?
    private var launch: StandaloneSandboxLaunch?
    private var root: URL?
    private var monitor: Task<Void, Never>?
    private var stopping = false
    private var didLaunch = false
    private(set) var lastFailure: String?
    var onFailure: (() -> Void)?
    var readyFile: URL? { root?.appendingPathComponent("ready.json") }
    var isRunning: Bool { process?.isRunning == true }

    nonisolated static func newSessionDirectory() -> URL {
        let identifier = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        return URL(fileURLWithPath: "/private/tmp/azki-esp-" + identifier.prefix(12))
    }

    func start(port: String, timeout: Int) async throws -> URL {
        lastFailure = nil
        try SimulatorSerialDevice.validatePath(port)
        guard process == nil, root == nil else { throw SimulatorError.transport("ESP32 上次会话尚未完成清理或数据提交") }
        let runtime = try await Task.detached { try StandaloneBundledRuntime.resolve() }.value
        try Task.checkCancellation()
        let directory = Self.newSessionDirectory()
        try StandaloneSandboxFiles.createPrivate(directory)
        root = directory
        let device = URL(fileURLWithPath: port)
        let policy = StandaloneProcessSandbox.policy(executables: [runtime.python],
            readTrees: [runtime.engine, runtime.script.deletingLastPathComponent()], readFiles: [],
            writeTrees: [directory], writeFiles: [device], unixDirectory: directory,
            extraRules: ["(allow file-ioctl (literal \(StandaloneProcessSandbox.quote(port))))"])
        let prepared = StandaloneSandboxLaunch(policyText: policy,
            arguments: [runtime.python.path, "-I", "-S", "-B", runtime.script.path,
                        "esp32-bridge", "--engine-root", runtime.engine.path, "--port", port,
                        "--state-root", directory.path], workingDirectory: directory, temporaryDirectory: directory)
        launch = prepared
        let fd = Darwin.open(directory.appendingPathComponent("bridge.log").path,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw SimulatorError.transport("无法创建 ESP32 桥接日志") }
        let log = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        output = log
        let child = Process()
        try prepared.configure(child)
        child.standardInput = FileHandle.nullDevice
        child.standardOutput = log; child.standardError = log
        process = child
        do {
            try child.run()
            didLaunch = true
            let deadline = ContinuousClock.now + .seconds(timeout)
            let ready = directory.appendingPathComponent("ready.json")
            while true {
                try Task.checkCancellation()
                guard child.isRunning else { throw SimulatorError.transport(failureMessage("ESP32 桥接启动失败")) }
                if let data = try? StandaloneSandboxFiles.regular(ready, limit: 16_384, privateOnly: true),
                   let value = try? StandaloneProcessSandbox.object(data), value["ready"] as? Bool == true,
                   value["port"] as? String == port,
                   let socket = value["hciSocket"] as? String,
                   URL(fileURLWithPath: socket).deletingLastPathComponent().path == directory.path {
                    monitor = Task { [weak self, weak child] in
                        while !Task.isCancelled, let child, child.isRunning {
                            try? await Task.sleep(for: .milliseconds(100))
                        }
                        guard !Task.isCancelled, let self, !self.stopping else { return }
                        self.onFailure?()
                    }
                    return ready
                }
                guard ContinuousClock.now < deadline else { throw SimulatorError.transport(failureMessage("ESP32 桥接握手超时")) }
                try await Task.sleep(for: .milliseconds(50))
            }
        } catch {
            let message = error.localizedDescription
            let closed = await stop()
            throw SimulatorError.transport(closed ? message : message + "\n" + (lastFailure ?? "串口清理尚未确认"))
        }
    }

    private func failureMessage(_ fallback: String) -> String {
        guard let root else { return fallback }
        for name in ["closed.json", "state.json"] {
            if let data = try? StandaloneSandboxFiles.regular(root.appendingPathComponent(name), limit: 65_536, privateOnly: true),
               let value = try? StandaloneProcessSandbox.object(data),
               let message = (value["message"] ?? value["firstError"] ?? value["error"]) as? String, !message.isEmpty {
                return fallback + "：" + String(message.prefix(1_024))
            }
        }
        if let data = try? StandaloneSandboxFiles.regular(root.appendingPathComponent("bridge.log"), limit: 1_048_576, privateOnly: true), !data.isEmpty {
            return fallback + "\n" + String(decoding: data.suffix(2_048), as: UTF8.self)
        }
        return fallback
    }

    nonisolated static func cleanupConfirmed(_ receipt: [String: Any]) -> Bool {
        receipt["closed"] as? Bool == true && receipt["stopConfirmed"] as? Bool == true
            && receipt["leaseReleased"] as? Bool == true && receipt["cleanupComplete"] as? Bool == true
            && (receipt["firstError"] is NSNull
                || (receipt["portOpened"] as? Bool == false && receipt["controllerStarted"] as? Bool == false))
    }

    @discardableResult func stop() async -> Bool {
        stopping = true
        monitor?.cancel(); monitor = nil
        if let process, process.isRunning, !(await StandaloneProcessReaper.stop(process, graceSeconds: 5)) {
            lastFailure = failureMessage("ESP32 桥接进程尚未退出；已保留会话记录")
            stopping = false
            return false
        }
        if didLaunch {
            guard let root,
                  let data = try? StandaloneSandboxFiles.regular(root.appendingPathComponent("closed.json"), limit: 65_536, privateOnly: true),
                  let receipt = try? StandaloneProcessSandbox.object(data),
                  Self.cleanupConfirmed(receipt) else {
                lastFailure = failureMessage("ESP32 的 STOP 和串口释放尚未确认；已保留会话，不能自动重开")
                stopping = false
                return false
            }
        }
        try? output?.close(); output = nil
        launch?.cleanup(); launch = nil; process = nil
        didLaunch = false; stopping = false
        return true
    }

    func archiveStoppedState() {
        guard process == nil, !didLaunch else { return }
        if let root {
            do {
                let history = BundledFirmwareService.libraryRoot.appendingPathComponent("esp32-history/" + root.lastPathComponent)
                try FileManager.default.createDirectory(at: history, withIntermediateDirectories: true,
                                                       attributes: [.posixPermissions: 0o700])
                for file in try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isRegularFileKey]) {
                    if try file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true {
                        try FileManager.default.copyItem(at: file, to: history.appendingPathComponent(file.lastPathComponent))
                    }
                }
                try FileManager.default.removeItem(at: root)
            } catch { /* Keep the owned directory if its evidence could not be archived. */ }
        }
        root = nil
    }
}
