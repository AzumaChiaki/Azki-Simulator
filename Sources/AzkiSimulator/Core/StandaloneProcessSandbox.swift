import Foundation
import CryptoKit
import CoreFoundation
import Darwin

/// The only launch description for packaged interpreters. The outer policy is
/// installed before dyld/Python; a failed sandbox never falls back to raw exec.
nonisolated struct StandaloneSandboxLaunch: Sendable {
    private let artifact = StandalonePolicyArtifact()
    let policyText: String
    let arguments: [String]
    let workingDirectory: URL
    let temporaryDirectory: URL
    init(policyText: String, arguments: [String], workingDirectory: URL, temporaryDirectory: URL) {
        self.policyText = policyText; self.arguments = arguments
        self.workingDirectory = workingDirectory; self.temporaryDirectory = temporaryDirectory
    }
    var policySHA256: String { StandaloneSandboxFiles.digest(Data(policyText.utf8)) }
    var executable: URL { URL(fileURLWithPath: "/usr/bin/sandbox-exec") }
    var environment: [String: String] {
        ["PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C", "HOME": temporaryDirectory.path,
         "TMPDIR": temporaryDirectory.path, "PYTHONDONTWRITEBYTECODE": "1",
         // Instance clocks follow the Mac's zone unless the user overrides it.
         "AZKI_HOST_TIMEZONE": TimeZone.current.identifier]
    }
    func configure(_ process: Process) throws {
        let policyURL = try artifact.publish(policyText, in: temporaryDirectory)
        process.executableURL = executable
        process.arguments = ["-f", policyURL.path] + arguments
        process.environment = environment
        process.currentDirectoryURL = workingDirectory
    }
    func cleanup() { artifact.cleanup() }
}

nonisolated private final class StandalonePolicyArtifact: @unchecked Sendable {
    private let lock = NSLock()
    private var published: (URL, dev_t, ino_t)?
    func publish(_ text: String, in directory: URL) throws -> URL {
        lock.lock(); defer { lock.unlock() }
        guard published == nil else { throw SimulatorError.unsafeFile("沙盒策略已用于一次启动") }
        let url = directory.appendingPathComponent("outer-" + UUID().uuidString.lowercased() + ".sb")
        let info = try StandaloneSandboxFiles.writeExclusive(Data(text.utf8), to: url)
        published = (url, info.st_dev, info.st_ino)
        return url
    }
    func cleanup() {
        lock.lock(); defer { lock.unlock() }
        guard let (url, device, inode) = published else { return }
        var info = stat()
        if lstat(url.path, &info) == 0, info.st_dev == device, info.st_ino == inode,
           info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1 { _ = unlink(url.path) }
        published = nil
    }
    deinit { cleanup() }
}

nonisolated enum StandaloneSandboxFiles {
    static func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func canonicalURL(_ path: String) throws -> URL {
        let url = URL(fileURLWithPath: path)
        guard path.hasPrefix("/"), !path.contains("\0"), path.utf8.count < 4_096,
              path == "/" || path.dropFirst().split(separator: "/", omittingEmptySubsequences: false)
                .allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
            throw SimulatorError.unsafeFile("沙盒路径不是规范绝对路径")
        }
        return url
    }
    static func directory(_ url: URL, privateOnly: Bool = false) throws -> stat {
        _ = try canonicalURL(url.path)
        let fd = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("沙盒目录缺失、不可读或包含链接") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFDIR,
              (info.st_uid == geteuid() || (!privateOnly && info.st_uid == 0)),
              privateOnly ? info.st_mode & 0o777 == 0o700 : info.st_mode & 0o022 == 0 else {
            throw SimulatorError.unsafeFile("沙盒目录所有者或权限无效")
        }
        return info
    }
    static func createPrivate(_ url: URL) throws {
        guard mkdir(url.path, 0o700) == 0 else {
            let failure = errno
            throw SimulatorError.unsafeFile("不能独占创建私有目录 \(url.lastPathComponent)：\(String(cString: strerror(failure)))（\(failure)）")
        }
        _ = try directory(url, privateOnly: true)
    }
    /// The library and its instance folders are this app's own data. A restore,
    /// Finder copy or migration can widen their mode; narrow it back to 0700
    /// instead of refusing to run. Foreign owners, links and group/other-writable
    /// directories are still rejected by `directory(_:privateOnly:)`.
    static func narrowPrivate(_ url: URL) {
        let fd = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { return }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFDIR, info.st_uid == geteuid(),
              info.st_mode & 0o022 == 0, info.st_mode & 0o7777 != 0o700 else { return }
        _ = fchmod(fd, 0o700)
    }
    static func ensureLibrary(_ url: URL) throws {
        // Native host creates missing ancestors; the interpreter is granted only
        // the final private library, never Application Support or its parent.
        if access(url.path, F_OK) == 0 { narrowPrivate(url); _ = try directory(url, privateOnly: true); return }
        let parent = url.deletingLastPathComponent()
        if access(parent.path, F_OK) != 0 { try ensureLibrary(parent) }
        else { _ = try directory(parent) }
        // Multiple instance models may prepare their first catalog at once.
        // A competing creator is acceptable only after validating the exact
        // final directory; never weaken exclusive session/run creation.
        if mkdir(url.path, 0o700) != 0, errno != EEXIST {
            let failure = errno
            throw SimulatorError.unsafeFile("不能创建固件库目录：\(String(cString: strerror(failure)))（\(failure)）")
        }
        _ = try directory(url, privateOnly: true)
    }
    static func regular(_ url: URL, limit: Int = 4_194_304, privateOnly: Bool = false) throws -> Data {
        _ = try canonicalURL(url.path)
        let fd = Darwin.open(url.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("沙盒输入缺失或包含链接") }
        defer { Darwin.close(fd) }
        var before = stat()
        guard fstat(fd, &before) == 0, before.st_mode & S_IFMT == S_IFREG, before.st_nlink == 1,
              (before.st_uid == geteuid() || before.st_uid == 0), before.st_mode & 0o022 == 0,
              !privateOnly || (before.st_uid == geteuid() && before.st_mode & 0o777 == 0o600),
              before.st_size >= 0, before.st_size <= limit else { throw SimulatorError.unsafeFile("沙盒输入属性无效") }
        var result = Data(), buffer = [UInt8](repeating: 0, count: 65_536)
        while true {
            try Task.checkCancellation()
            let n = Darwin.read(fd, &buffer, buffer.count)
            if n < 0 && errno == EINTR { continue }
            guard n >= 0 else { throw SimulatorError.unsafeFile("沙盒输入读取失败") }
            if n == 0 { break }
            result.append(contentsOf: buffer.prefix(n))
            guard result.count <= limit else { throw SimulatorError.unsafeFile("沙盒输入超出限制") }
        }
        var after = stat()
        guard fstat(fd, &after) == 0, before.st_dev == after.st_dev, before.st_ino == after.st_ino,
              before.st_size == after.st_size, before.st_mode == after.st_mode,
              before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec,
              before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
              before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec,
              before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec, result.count == before.st_size else {
            throw SimulatorError.unsafeFile("沙盒输入在读取时发生变化")
        }
        return result
    }
    /// Validate a declared input without reading its contents. Code identity is
    /// enforced by macOS signatures; these checks constrain sandbox grants.
    static func validateRegular(_ url: URL, expectedBytes: Int, limit: Int) throws {
        _ = try canonicalURL(url.path)
        let fd = Darwin.open(url.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("沙盒输入缺失或包含链接") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1,
              (info.st_uid == geteuid() || info.st_uid == 0), info.st_mode & 0o022 == 0,
              expectedBytes >= 0, expectedBytes <= limit, info.st_size == expectedBytes else {
            throw SimulatorError.unsafeFile("沙盒输入属性无效")
        }
    }
    @discardableResult static func writeExclusive(_ data: Data, to url: URL) throws -> stat {
        let fd = Darwin.open(url.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW_ANY | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("不能独占创建沙盒文件") }
        defer { Darwin.close(fd) }
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let n = Darwin.write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if n < 0 && errno == EINTR { continue }
                guard n > 0 else { throw SimulatorError.unsafeFile("沙盒文件写入失败") }
                offset += n
            }
        }
        guard fsync(fd) == 0 else { throw SimulatorError.unsafeFile("沙盒文件同步失败") }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1,
              info.st_uid == geteuid(), info.st_mode & 0o777 == 0o600 else {
            throw SimulatorError.unsafeFile("沙盒文件属性改变")
        }
        return info
    }
    static func reserve(_ root: URL) throws -> String {
        try createPrivate(root)
        let info = try directory(root, privateOnly: true)
        let nonce = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        let data = try JSONSerialization.data(withJSONObject: ["version": 1, "nonce": nonce,
            "device": NSNumber(value: info.st_dev), "inode": NSNumber(value: info.st_ino)])
        try writeExclusive(data, to: root.appendingPathComponent(".reservation.json"))
        return nonce
    }
    static func child(_ path: String, of root: URL) -> Bool { path.hasPrefix(root.path + "/") }
}

/// Shared limits for long-lived local sessions. Pins stream through SHA-256, so
/// large generated YAFFS images do not need to fit in memory.
nonisolated enum StandaloneRunLimits {
    static let unlimited = 0
    static let maximumSeconds = 28_800
    static let maximumPinnedFileBytes = 1_073_741_824
    static let maximumPinnedTotalBytes = 6_442_450_944
    static func isValid(_ seconds: Int) -> Bool { (unlimited...maximumSeconds).contains(seconds) }
    /// Zero supplies no limit; a positive prepared/test limit cannot be extended
    /// by a zero or a larger override from another layer.
    static func effective(_ limits: Int...) -> Int {
        precondition(limits.allSatisfy(isValid))
        return limits.filter { $0 > unlimited }.min() ?? unlimited
    }
    static func deadline(seconds: Int, now: ContinuousClock.Instant = .now, graceSeconds: Int = 0) -> ContinuousClock.Instant? {
        precondition(isValid(seconds) && graceSeconds >= 0)
        return seconds == unlimited ? nil : now + .seconds(seconds + graceSeconds)
    }
    /// "45 秒", "12 分钟", "8 小时" or "1 小时 30 分钟"; used for limits and remaining time.
    static func describe(_ seconds: Int) -> String {
        if seconds == unlimited { return "不限时" }
        let value = max(0, seconds)
        if value < 60 { return "\(value) 秒" }
        if value < 3_600 { return "\((value + 59) / 60) 分钟" }
        let hours = value / 3_600, minutes = (value % 3_600 + 59) / 60
        return minutes == 0 || minutes == 60 ? "\(hours + (minutes == 60 ? 1 : 0)) 小时" : "\(hours) 小时 \(minutes) 分钟"
    }
    static func validProfileID(_ value: String) -> Bool {
        value.utf8.count <= 96 && value.range(of: #"^[a-z0-9._]+@[0-9][0-9.]*(@[0-9a-f]{12})?$"#, options: .regularExpression) != nil
    }
}

nonisolated enum StandaloneProcessSandbox {
    static let version = 1
    // JSON string encoding is also a valid SBPL quoted string. Never interpolate
    // unescaped user paths or invoke a shell.
    static func quote(_ value: String) -> String {
        let data = try! JSONSerialization.data(withJSONObject: [value], options: [.withoutEscapingSlashes])
        return String(data: data, encoding: .utf8)!.dropFirst().dropLast().description
    }
    /// `extraRules` carries complete SBPL rules built only from constants in this
    /// file (e.g. a name-prefix regex that `subpath` cannot express); never user input.
    static func policy(executables: [URL], readTrees: [URL], readFiles: [URL], writeTrees: [URL],
                       writeFiles: [URL] = [], unixDirectory: URL? = nil, allowChildren: Bool = false,
                       extraRules: [String] = []) -> String {
        var rules = ["(version 1)", "(deny default)", "(allow sysctl-read)",
            // The local dyld libignition bootstrap opens the root as an openat
            // anchor. This grants only the directory itself, not its children.
            "(allow file-read* (literal \"/\"))",
            "(allow file-read* (subpath \"/System/Library\") (subpath \"/usr/lib\"))",
            "(allow file-read* (literal \"/dev/null\") (literal \"/dev/random\") (literal \"/dev/urandom\"))",
            "(allow file-write* (literal \"/dev/null\"))"]
        for url in Set(executables) { rules.append("(allow process-exec (literal \(quote(url.path))))") }
        for url in Set(readTrees) { rules.append("(allow file-read* (subpath \(quote(url.path))))") }
        for url in Set(readFiles + executables) { rules.append("(allow file-read* (literal \(quote(url.path))))") }
        for url in Set(writeTrees) { rules.append("(allow file-read* file-write* (subpath \(quote(url.path))))") }
        for url in Set(writeFiles) { rules.append("(allow file-read* file-write* (literal \(quote(url.path))))") }
        // Traversal metadata is bounded to exact ancestors, never their contents.
        var parents = Set<String>()
        for url in readTrees + readFiles + writeTrees + writeFiles + executables {
            var parent = url.deletingLastPathComponent()
            while parent.path != "/" { parents.insert(parent.path); parent.deleteLastPathComponent() }
        }
        parents.insert("/")
        for path in parents.sorted() { rules.append("(allow file-read-metadata (literal \(quote(path))))") }
        if allowChildren {
            rules += ["(allow process-fork)", "(allow process-info*)", "(allow signal (target self) (target children))"]
        }
        if let unixDirectory {
            rules.append("(allow network-bind network-inbound network-outbound (local unix-socket (subpath \(quote(unixDirectory.path)))))")
        }
        rules += extraRules
        return rules.prefix(2).joined(separator: "\n") + "\n" + rules.dropFirst(2).sorted().joined(separator: "\n") + "\n"
    }

    static func makeInspection(runtime: StandaloneBundledRuntime, arguments: [String], library: URL,
                               temporary: URL) throws -> StandaloneSandboxLaunch {
        try Task.checkCancellation()
        _ = try StandaloneSandboxFiles.directory(temporary, privateOnly: true)
        try StandaloneSandboxFiles.ensureLibrary(library)
        var argv = arguments
        var inputs: [URL] = []
        var writes = [temporary]
        var extraRules: [String] = []
        guard let command = arguments.first else { throw SimulatorError.transport("缺少静态命令") }
        switch command {
        case "library-catalog":
            guard arguments.count == 1 else { throw SimulatorError.transport("目录命令参数无效") }
        case "library-delete":
            // Removes an imported OTA object and rewrites the library index; it
            // writes only inside the firmware library root.
            guard arguments.count == 3, arguments[1] == "--sha256",
                  FirmwareLibraryValidation.digest(arguments[2]) else {
                throw SimulatorError.transport("删除参数无效")
            }
            writes.append(library)
        case "library-delete-all":
            // Clears every imported firmware and rewrites the library index; it
            // writes only inside the firmware library root.
            guard arguments.count == 1 else { throw SimulatorError.transport("清空参数无效") }
            writes.append(library)
        case "library-import", "library-probe":
            // library-import optionally binds the OTA to the user's selected
            // hardware model: ["library-import", "--input", PATH, "--model", MODEL].
            guard arguments.count == 3 || arguments.count == 5,
                  arguments[1] == "--input",
                  arguments.count == 3 || arguments[3] == "--model" else {
                throw SimulatorError.transport("导入参数无效")
            }
            let url = try StandaloneSandboxFiles.canonicalURL(arguments[2])
            // Validate type/owner without loading a potentially large OTA.
            try validateInput(url)
            inputs.append(url)
            if command == "library-import" {
                if arguments.count == 5 {
                    guard FirmwareLibraryValidation.identifier(arguments[4]) else {
                        throw SimulatorError.transport("导入机型参数无效")
                    }
                }
                writes.append(library)
            }
        case "prepare-bundled":
            let arguments = ["--skip-oobe", "--no-skip-oobe"].contains(arguments.last ?? "")
                ? Array(arguments.dropLast()) : arguments
            guard [5, 7, 11].contains(arguments.count), arguments[1] == "--profile-id", FirmwareLibraryValidation.identifier(arguments[2]),
                  arguments[3] == "--session-root" else { throw SimulatorError.transport("准备参数无效") }
            if arguments.count >= 7 {
                guard arguments[5] == "--instance-id", let id = UUID(uuidString: arguments[6]),
                      id.uuidString.lowercased() == arguments[6] else { throw SimulatorError.transport("实例身份无效") }
                let instance = library.appendingPathComponent("instances/" + arguments[6], isDirectory: true)
                try StandaloneSandboxFiles.ensureLibrary(instance)
                writes.append(instance)
                // Settles an interrupted earlier run of this instance itself.
                extraRules = preparedSessionRules
            }
            if arguments.count == 11 {
                guard arguments[7] == "--virtual-serial", arguments[9] == "--virtual-address" else {
                    throw SimulatorError.transport("虚拟身份参数无效")
                }
                _ = try SimulatorVirtualIdentity(serial: arguments[8], address: arguments[10]).validated(model: "")
            }
            let root = try reservationRoot(arguments[4], prefix: "azki-b-")
            let nonce = try StandaloneSandboxFiles.reserve(root)
            argv += ["--reservation-nonce", nonce]; writes.append(root)
            if arguments.count >= 7, try permitsSyntheticKey(runtime: runtime, profile: arguments[2]) {
                // SecKey random generation cannot run in the static helper's
                // deny-default sandbox. Generate only our fresh ephemeral key
                // in the native host; the helper imports this bounded snapshot.
                // No Keychain item, account, radio or sandbox exception is used.
                let key = P256.KeyAgreement.PrivateKey().x963Representation
                guard key.count == 97, key.first == 4 else {
                    throw SimulatorError.transport("无法生成本次虚拟身份的临时密钥")
                }
                let keyFile = temporary.appendingPathComponent("native-p256.bin")
                try StandaloneSandboxFiles.writeExclusive(key, to: keyFile)
                argv += ["--virtual-key-file", keyFile.path, "--virtual-key-sha256", StandaloneSandboxFiles.digest(key)]
            }
        case "instance-identity":
            guard arguments.count == 3, arguments[1] == "--instance-id",
                  let id = UUID(uuidString: arguments[2]), id.uuidString.lowercased() == arguments[2] else {
                throw SimulatorError.transport("实例身份读取参数无效")
            }
        case "instance-settings":
            guard [3, 5].contains(arguments.count), arguments[1] == "--instance-id",
                  let id = UUID(uuidString: arguments[2]), id.uuidString.lowercased() == arguments[2],
                  arguments.count == 3 || (arguments[3] == "--set" && arguments[4].utf8.count <= 262_144) else {
                throw SimulatorError.transport("实例设置参数无效")
            }
            let instance = library.appendingPathComponent("instances/" + arguments[2], isDirectory: true)
            try StandaloneSandboxFiles.ensureLibrary(instance)
            writes.append(instance)
        case "instance-reset":
            // Recovers this instance's interrupted commits/staging and clears its
            // session caches; it may write only inside the instance's own folder.
            guard arguments.count == 3, arguments[1] == "--instance-id",
                  let id = UUID(uuidString: arguments[2]), id.uuidString.lowercased() == arguments[2] else {
                throw SimulatorError.transport("实例重置参数无效")
            }
            let instance = library.appendingPathComponent("instances/" + arguments[2], isDirectory: true)
            try StandaloneSandboxFiles.ensureLibrary(instance)
            writes.append(instance)
            // A stopped run's session can then be committed instead of rolled back.
            extraRules = preparedSessionRules
        case "sessions-cleanup":
            // Removes stale session roots this app created in /private/tmp. SBPL
            // `subpath` cannot express a name prefix, so the grant is a fixed regex
            // over our own prefixes; listing the parent reveals names only.
            guard arguments.count == 1 else { throw SimulatorError.transport("会话清理参数无效") }
            extraRules = sessionCleanupRules
        case "instance-storage-finish":
            guard arguments.count == 5, arguments[1] == "--session", arguments[3] == "--instance-id",
                  let id = UUID(uuidString: arguments[4]), id.uuidString.lowercased() == arguments[4] else {
                throw SimulatorError.transport("实例数据提交参数无效")
            }
            let session = try StandaloneSandboxFiles.canonicalURL(arguments[2])
            guard session.lastPathComponent == "session",
                  session.deletingLastPathComponent().deletingLastPathComponent().path == "/private/tmp",
                  session.deletingLastPathComponent().lastPathComponent.hasPrefix("azki-b-") else {
                throw SimulatorError.unsafeFile("实例提交目录不是本应用准备的会话")
            }
            _ = try StandaloneSandboxFiles.directory(session, privateOnly: true)
            let instance = library.appendingPathComponent("instances/" + arguments[4], isDirectory: true)
            StandaloneSandboxFiles.narrowPrivate(instance)
            _ = try StandaloneSandboxFiles.directory(instance, privateOnly: true)
            writes += [session, instance]
        case "prepare":
            guard arguments.count == 8, arguments[2] == "--qemu", arguments[4] == "--firmware", arguments[6] == "--resource" else {
                throw SimulatorError.transport("高级准备参数无效")
            }
            let root = try reservationRoot(arguments[1], prefix: "azki-sim-")
            for index in [3, 5, 7] {
                let url = try StandaloneSandboxFiles.canonicalURL(arguments[index]); try validateInput(url); inputs.append(url)
            }
            argv += ["--reservation-nonce", try StandaloneSandboxFiles.reserve(root)]; writes.append(root)
        default: throw SimulatorError.transport("未授权的静态命令")
        }
        if command != "prepare" { argv += ["--engine-root", runtime.engine.path, "--library-root", library.path] }
        let trees = try packagedReadTrees(runtime)
        // Prepare derives an imported firmware's NOR layout by forking the
        // bundled Python (multi_device --nor-layout); deny-default blocks fork
        // unless granted, which would silently disable persistent NOR and the
        // virtual peer for every generated profile.
        let allowChildren = command == "prepare-bundled" || command == "prepare"
        let text = policy(executables: [runtime.python], readTrees: trees + [library], readFiles: inputs, writeTrees: writes,
                          allowChildren: allowChildren, extraRules: extraRules)
        return StandaloneSandboxLaunch(policyText: text, arguments: [runtime.python.path, "-I", "-S", "-B", runtime.script.path] + argv,
            workingDirectory: temporary, temporaryDirectory: temporary)
    }

    /// Prepared sessions only: lets instance recovery commit an earlier stopped run.
    static let preparedSessionRules = [
        "(allow file-read-metadata (literal \"/private/tmp\") (literal \"/tmp\") (literal \"/private\"))",
        "(allow file-read* file-write* (regex #\"^/private/tmp/azki-b-\"))",
    ]

    /// Session roots created by this app: prepared (`azki-b-`), advanced
    /// (`azki-sim-`) and inspection temporaries (`azki-cmd-`).
    static let sessionCleanupRules = [
        "(allow file-read* (literal \"/private/tmp\"))",
        "(allow file-read-metadata (literal \"/tmp\") (literal \"/private\"))",
        "(allow file-read* file-write* (regex #\"^/private/tmp/azki-(b|sim|cmd)-\"))",
    ]

    private static func permitsSyntheticKey(runtime: StandaloneBundledRuntime, profile: String) throws -> Bool {
        // Capabilities are selected by profile and bounded layout. A historical
        // executable digest is descriptive release metadata, not permission.
        let manifest = try object(StandaloneSandboxFiles.regular(runtime.engine.appendingPathComponent("manifest.json"),
            limit: 2_097_152))
        guard let layouts = try? StandaloneVirtualPeerBinding.validatedLayouts(capabilities: manifest) else {
            return false
        }
        // Firmware outside the packaged table (another catalog version or an
        // imported OTA of a supported preset) gets its AP-derived layout from
        // the helper, which decides whether the identity is enabled.
        return layouts[profile] != nil
            || profile.range(of: #"^miwear\.watch\.(n66|n67|o66|p65|p67|q65|q66)[a-z]{0,8}@[0-9][0-9.]{0,31}@[0-9a-f]{12}$"#,
                             options: .regularExpression) != nil
    }

    static func makeServe(runtime: StandaloneBundledRuntime, session: URL, approvedIdentity: String,
                          maxRuntime: Int, library: URL, temporary: URL, guestRun: URL? = nil,
                          debugger: StandaloneDebugger? = nil) throws -> StandaloneSandboxLaunch {
        guard FirmwareLibraryValidation.digest(approvedIdentity), StandaloneRunLimits.isValid(maxRuntime),
              debugger?.isValid ?? true else {
            throw SimulatorError.transport("缺少本次完整运行身份")
        }
        _ = try StandaloneSandboxFiles.directory(session, privateOnly: true)
        _ = try StandaloneSandboxFiles.directory(temporary, privateOnly: true)
        let descriptor = try object(StandaloneSandboxFiles.regular(session.appendingPathComponent("session.json")))
        let plan = try object(StandaloneSandboxFiles.regular(session.appendingPathComponent("backend-plan.json")))
        guard descriptor["backend"] as? String == "minative", descriptor["directory"] as? String == session.path,
              descriptor["approvalDigest"] as? String == approvedIdentity,
              plan["approvalDigest"] as? String == approvedIdentity, let identity = plan["identity"] as? [String: Any],
              StandaloneSandboxFiles.digest(try canonicalJSON(identity)) == approvedIdentity else {
            throw SimulatorError.transport("会话计划与本次批准身份不一致")
        }
        let prepared = session.deletingLastPathComponent().appendingPathComponent("prepared", isDirectory: true)
        let project = prepared.appendingPathComponent("project", isDirectory: true)
        _ = try StandaloneSandboxFiles.directory(prepared, privateOnly: true)
        guard identity["project"] as? String == project.path, let python = identity["python"] as? [String: Any],
              python["invocation"] as? String == runtime.python.path,
              let qemu = identity["qemu"] as? [String: Any], let qemuPath = qemu["path"] as? String,
              qemuPath == project.appendingPathComponent("backend/build/qemu-system-arm").path else {
            throw SimulatorError.transport("会话启动入口超出内置计划")
        }
        let trees = try packagedReadTrees(runtime)
        let roots = trees + [library, prepared]
        var pins: [URL] = []
        var seenPins: [String: Int] = [:]
        var totalBytes = 0
        var count = 0
        func visit(_ value: Any, depth: Int) throws {
            guard depth <= 32 else { throw SimulatorError.transport("计划嵌套超出限制") }
            count += 1; guard count <= 100_000 else { throw SimulatorError.transport("计划超出限制") }
            if let row = value as? [String: Any] {
                // Relative paths grant no permission. Descriptive SHA metadata
                // is not a startup gate; absolute paths and sizes define grants.
                if let path = row["path"] as? String, path.hasPrefix("/"),
                   let bytes = row["bytes"] as? Int {
                    let url = try StandaloneSandboxFiles.canonicalURL(path)
                    guard roots.contains(where: { StandaloneSandboxFiles.child(path, of: $0) }),
                          (0...StandaloneRunLimits.maximumPinnedFileBytes).contains(bytes) else {
                        throw SimulatorError.transport("计划文件超出本次沙盒范围")
                    }
                    if let previous = seenPins[path] {
                        guard previous == bytes else { throw SimulatorError.transport("计划含冲突的文件尺寸") }
                    } else {
                        totalBytes += bytes
                        guard seenPins.count < 8_192, totalBytes <= StandaloneRunLimits.maximumPinnedTotalBytes else { throw SimulatorError.transport("计划文件总量超出限制") }
                        try StandaloneSandboxFiles.validateRegular(url, expectedBytes: bytes,
                            limit: StandaloneRunLimits.maximumPinnedFileBytes)
                        seenPins[path] = bytes; pins.append(url)
                    }
                }
                for child in row.values { try visit(child, depth: depth + 1) }
            } else if let rows = value as? [Any] { for child in rows { try visit(child, depth: depth + 1) } }
        }
        try visit(identity, depth: 0)
        // Compare paths, not URL values: equal paths can differ in URL representation.
        let pinnedPaths = Set(pins.map { $0.standardizedFileURL.resolvingSymlinksInPath().path })
        func pinned(_ url: URL) -> Bool { pinnedPaths.contains(url.standardizedFileURL.resolvingSymlinksInPath().path) }
        guard !pins.isEmpty, pinned(URL(fileURLWithPath: qemuPath)), pinned(runtime.python) else {
            throw SimulatorError.transport("计划缺少执行文件路径")
        }
        var directories = Set<URL>()
        for pin in pins {
            var parent = pin.deletingLastPathComponent()
            while roots.contains(where: { parent.path == $0.path || StandaloneSandboxFiles.child(parent.path, of: $0) }) {
                _ = try StandaloneSandboxFiles.directory(parent)
                directories.insert(parent); parent.deleteLastPathComponent()
            }
        }
        if let run = guestRun {
            guard run.deletingLastPathComponent() == session, StandaloneBrokerConfiguration.validRun(run.lastPathComponent),
                  let profile = plan["profileID"] as? String, StandaloneRunLimits.validProfileID(profile),
                  let storage = identity["storageKind"] as? String, storage == "romfs" || storage == "yaffs",
                  identity["profile"] is [String: Any],
                  let spec = plan["spec"] as? [String: Any],
                  spec["bootstrap"] as? String == runtime.engine.appendingPathComponent("bootstrap.py").path,
                  spec["project"] as? String == project.path else { throw SimulatorError.transport("原生客体计划入口不匹配") }
            _ = try StandaloneSandboxFiles.directory(run, privateOnly: true)
            let guestTemporary = run.appendingPathComponent("tmp", isDirectory: true)
            _ = try StandaloneSandboxFiles.directory(guestTemporary, privateOnly: true)
            let supervisor = runtime.script.deletingLastPathComponent().appendingPathComponent("azki_simulator/minative_launch.py")
            guard pinned(supervisor) else { throw SimulatorError.transport("原生客体监督器未纳入启动计划") }
            var guestExecutables = [runtime.python, URL(fileURLWithPath: qemuPath)]
            var guestWrites = [run]
            var instanceReads: [URL] = []
            var instanceWrites: [URL] = []
            var instanceArguments: [String] = []
            var settingsArguments: [String] = []
            if let binding = identity["instanceStorage"] as? [String: Any], binding["available"] as? Bool == true {
                guard let capabilities = identity["runtimeCapabilities"] as? [String: Any],
                      let capability = capabilities["nandPersistence"] as? [String: Any],
                      capability["schema"] as? Int == 1,
                      binding["sessionRoot"] as? String == session.path,
                      binding["profileID"] as? String == profile,
                      let bytes = binding["dataBytes"] as? Int, bytes > 0,
                      bytes <= StandaloneRunLimits.maximumPinnedFileBytes, bytes.isMultiple(of: 2048) else {
                    throw SimulatorError.transport("实例数据盘与核心身份不匹配")
                }
                let root = session.appendingPathComponent("instance-data", isDirectory: true)
                _ = try StandaloneSandboxFiles.directory(root, privateOnly: true)
                for (role, file, hash, count, flag) in [
                    ("dataImage", "nand-data.bin", "dataSHA256", bytes, "--instance-data-image"),
                    ("spareImage", "nand-spare.bin", "spareSHA256", bytes / 2048 * 64, "--instance-spare-image")
                ] {
                    let url = root.appendingPathComponent(file)
                    var info = stat()
                    guard binding[role] as? String == url.path, let digest = binding[hash] as? String,
                          FirmwareLibraryValidation.digest(digest), lstat(url.path, &info) == 0,
                          info.st_uid == geteuid(), info.st_mode & 0o777 == 0o600,
                          info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1 else {
                        throw SimulatorError.unsafeFile("实例工作盘属性无效")
                    }
                    try StandaloneSandboxFiles.validateRegular(url, expectedBytes: count,
                        limit: StandaloneRunLimits.maximumPinnedFileBytes)
                    instanceWrites.append(url)
                    instanceArguments += [flag, url.path]
                }
                instanceArguments += ["--instance-data-sha256", binding["dataSHA256"] as! String,
                                      "--instance-spare-sha256", binding["spareSHA256"] as! String]
                if let nor = try StandaloneNorBinding.resolve(binding: binding, capabilities: capabilities,
                    profile: profile, session: session) {
                    instanceWrites.append(nor.writeFile)
                    instanceArguments += nor.arguments
                }
                if let peer = try StandaloneVirtualPeerBinding.resolve(binding: binding, capabilities: capabilities,
                    profile: profile, session: session, run: run) {
                    instanceReads.append(peer.readFile)
                    instanceArguments += peer.arguments
                }
                if let path = binding["settingsPath"] as? String {
                    // Host-authored firmware settings, applied by the runner before boot.
                    let settings = root.appendingPathComponent("instance-settings.json")
                    var info = stat()
                    guard path == settings.path,
                          let digest = binding["settingsSHA256"] as? String, FirmwareLibraryValidation.digest(digest),
                          lstat(settings.path, &info) == 0, info.st_uid == geteuid(), info.st_mode & 0o777 == 0o600,
                          info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1, info.st_size <= 1_048_576 else {
                        throw SimulatorError.unsafeFile("实例固件设置文件属性无效")
                    }
                    settingsArguments = ["--instance-settings", settings.path]
                    instanceReads.append(settings)
                }
            }
            if storage == "yaffs" {
                // The YAFFS image builders run inside the same sandbox and the
                // generated NAND caches live under this session's private project.
                let builders = ["yaffs-image", "legacy-ui-db"].map { project.appendingPathComponent("backend/build/" + $0) }
                guard builders.allSatisfy({ builder in pinned(builder) }) else {
                    throw SimulatorError.transport("YAFFS 构建器未纳入启动计划")
                }
                guestExecutables += builders
            }
            // /data is YAFFS on every model. Older engines lack the settings tool;
            // it is granted only when pinned.
            let properties = project.appendingPathComponent("backend/build/instance-props")
            if pinned(properties) { guestExecutables.append(properties) }
            else if !settingsArguments.isEmpty { throw SimulatorError.transport("实例设置工具未纳入启动计划") }
            // Every profile derives its storage caches (YAFFS NAND images, ROMFS
            // resources extracted from the OTA) under this session's private project.
            let assets = project.appendingPathComponent("assets", isDirectory: true)
            if access(assets.path, F_OK) != 0 { try StandaloneSandboxFiles.createPrivate(assets) }
            _ = try StandaloneSandboxFiles.directory(assets, privateOnly: true)
            guestWrites.append(assets)
            let text = policy(executables: guestExecutables, readTrees: [],
                readFiles: pins + Array(directories) + instanceReads, writeTrees: guestWrites, writeFiles: instanceWrites,
                unixDirectory: run, allowChildren: true, extraRules: debugger.map { [$0.sandboxRule] } ?? [])
            return StandaloneSandboxLaunch(policyText: text, arguments: [runtime.python.path, "-I", "-S", "-B", supervisor.path,
                "--native", String(maxRuntime), run.appendingPathComponent("runner-process.json").path,
                String(getpid()), runtime.engine.path, project.path, profile, run.path] + instanceArguments + settingsArguments
                    + [(plan["skipOOBE"] as? Bool ?? true) ? "--skip-oobe" : "--no-skip-oobe"] + (debugger?.arguments ?? []),
                workingDirectory: project, temporaryDirectory: guestTemporary)
        }
        // H may inspect peer/process ownership, but never forks or executes G.
        // The native app creates G as a separate sibling under its narrower policy.
        let text = policy(executables: [runtime.python], readTrees: [], readFiles: pins + Array(directories),
                          writeTrees: [session, temporary], unixDirectory: session) + "(allow process-info*)\n"
        return StandaloneSandboxLaunch(policyText: text, arguments: [runtime.python.path, "-I", "-S", "-B", runtime.script.path,
            "serve", session.path, "--max-runtime", String(maxRuntime), "--approved-identity-sha256", approvedIdentity, "--no-discovery", "--native-broker"],
            workingDirectory: session, temporaryDirectory: temporary)
    }

    static func object(_ data: Data) throws -> [String: Any] {
        guard let result = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw SimulatorError.transport("计划 JSON 无效") }
        return result
    }
    private static func reservationRoot(_ path: String, prefix: String) throws -> URL {
        let url = try StandaloneSandboxFiles.canonicalURL(path)
        guard url.deletingLastPathComponent().path == "/private/tmp", url.lastPathComponent.hasPrefix(prefix),
              url.lastPathComponent.utf8.count <= 40 else { throw SimulatorError.unsafeFile("准备目录超出私有根范围") }
        return url
    }
    private static func validateInput(_ url: URL) throws {
        let fd = Darwin.open(url.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.unsafeFile("选中文件缺失或包含链接") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1,
              (info.st_uid == geteuid() || info.st_uid == 0), info.st_mode & 0o022 == 0,
              info.st_size > 0, info.st_size <= 805_306_368 else { throw SimulatorError.unsafeFile("选中文件属性无效") }
    }
    private static func packagedReadTrees(_ runtime: StandaloneBundledRuntime) throws -> [URL] {
        var trees = [runtime.engine, runtime.script.deletingLastPathComponent()]
        let firmware = runtime.engine.deletingLastPathComponent().appendingPathComponent("Firmware", isDirectory: true)
        if access(firmware.path, F_OK) == 0 { trees.append(firmware) }
        for tree in trees { _ = try StandaloneSandboxFiles.directory(tree) }
        return trees
    }

    /// Matches Python json.dumps(sort_keys=True,separators=(',',':'),ensure_ascii=True).
    /// Plan identities contain integral JSON only; unknown numeric forms fail closed.
    static func canonicalJSON(_ value: Any) throws -> Data {
        var nodes = 0
        func encode(_ value: Any, depth: Int) throws -> String {
            nodes += 1
            guard depth <= 32, nodes <= 100_000 else { throw SimulatorError.transport("身份 JSON 超出限制") }
            if value is NSNull { return "null" }
            if let string = value as? String {
                var result = "\""
                for unit in string.utf16 {
                    switch unit {
                    case 34: result += "\\\""
                    case 92: result += "\\\\"
                    case 8: result += "\\b"
                    case 9: result += "\\t"
                    case 10: result += "\\n"
                    case 12: result += "\\f"
                    case 13: result += "\\r"
                    case 32...126: result.append(Character(UnicodeScalar(unit)!))
                    default: result += String(format: "\\u%04x", unit)
                    }
                }
                return result + "\""
            }
            if let number = value as? NSNumber {
                if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue ? "true" : "false" }
                guard !["f", "d"].contains(String(cString: number.objCType)) else { throw SimulatorError.transport("身份含不支持的数值") }
                return number.stringValue
            }
            if let object = value as? [String: Any] {
                return "{" + (try object.keys.sorted { $0.unicodeScalars.lexicographicallyPrecedes($1.unicodeScalars) }.map {
                    try encode($0, depth: depth + 1) + ":" + encode(object[$0]!, depth: depth + 1)
                }).joined(separator: ",") + "}"
            }
            if let rows = value as? [Any] { return "[" + (try rows.map { try encode($0, depth: depth + 1) }).joined(separator: ",") + "]" }
            throw SimulatorError.transport("身份含未知 JSON 类型")
        }
        return Data(try encode(value, depth: 0).utf8)
    }
}

/// Serial actor bookkeeping, concurrent detached I/O. Cancellation before start
/// and Process.run share the same lock, so a cancelled job cannot launch later.
actor StandaloneInspectionExecutor {
    private struct Job { let task: Task<Data, Error>; let owner: StandaloneOwnedCommand }
    private var jobs: [UUID: Job] = [:]
    func run(_ arguments: [String]) async throws -> Data {
        try Task.checkCancellation()
        let id = UUID(), owner = StandaloneOwnedCommand()
        let task = Task.detached(priority: .userInitiated) {
            let runtime = try StandaloneBundledRuntime.resolve()
            try Task.checkCancellation()
            let temporary = URL(fileURLWithPath: "/private/tmp/azki-cmd-" + UUID().uuidString.lowercased().prefix(12))
            try StandaloneSandboxFiles.createPrivate(temporary)
            let temporaryIdentity = try StandaloneSandboxFiles.directory(temporary, privateOnly: true)
            // Never recursively delete interpreter-created unknown files. Empty
            // temp directories are removed; nonempty leftovers remain private.
            defer {
                if let current = try? StandaloneSandboxFiles.directory(temporary, privateOnly: true),
                   current.st_dev == temporaryIdentity.st_dev, current.st_ino == temporaryIdentity.st_ino {
                    _ = Darwin.rmdir(temporary.path)
                }
            }
            let launch = try StandaloneProcessSandbox.makeInspection(runtime: runtime, arguments: arguments,
                library: BundledFirmwareService.libraryRoot, temporary: temporary)
            defer { launch.cleanup() }
            try Task.checkCancellation()
            return try StandalonePython.run(launch: launch, command: arguments[0], owner: owner)
        }
        jobs[id] = Job(task: task, owner: owner)
        defer { jobs[id] = nil }
        return try await withTaskCancellationHandler(operation: { try await task.value }, onCancel: { task.cancel(); owner.cancel() })
    }
    func cancelAll() async {
        let pending = Array(jobs.values)
        pending.forEach { $0.task.cancel(); $0.owner.cancel() }
        for job in pending { _ = try? await job.task.value }
    }
}

nonisolated final class StandaloneOwnedCommand: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var process: Process?
    func start(_ process: Process) throws {
        lock.lock(); defer { lock.unlock() }
        guard !cancelled else { throw CancellationError() }
        self.process = process
        try process.run()
    }
    func cancel() {
        lock.lock(); cancelled = true; let child = process; lock.unlock()
        guard let child, child.isRunning else { return }
        child.terminate()
        DispatchQueue.global().asyncAfter(deadline: .now() + 1) {
            if child.isRunning { Darwin.kill(child.processIdentifier, SIGKILL) }
        }
    }
    func checkCancellation() throws { lock.lock(); defer { lock.unlock() }; if cancelled { throw CancellationError() } }
}
