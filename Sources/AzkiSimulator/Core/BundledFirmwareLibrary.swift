import Foundation
import Darwin

/// The UI consumes catalog metadata only. Asset matching, imports and identity
/// preparation belong to the app's own audited CLI; no package code is loaded.
nonisolated struct BundledFirmwareItem: Codable, Identifiable, Equatable, Sendable {
    struct Availability: Codable, Equatable, Sendable {
        let ready: Bool
        let reason: String
        let missingRoles: [String]
    }
    enum Source: String, Codable, Sendable { case bundled, imported }
    let id: String
    let displayName: String
    let device: String
    let model: String
    let firmwareVersion: String
    let width: Int
    let height: Int
    let availability: Availability
    let source: Source

    func validate() throws {
        guard FirmwareLibraryValidation.identifier(id),
              [displayName, device, model, firmwareVersion].allSatisfy({ FirmwareLibraryValidation.text($0, limit: 240) }),
              [(192, 490), (212, 520), (336, 480)].contains(where: { $0 == (width, height) }),
              FirmwareLibraryValidation.text(availability.reason, limit: 1_024, empty: true),
              availability.missingRoles.count <= 32,
              availability.missingRoles.allSatisfy({ FirmwareLibraryValidation.identifier($0) }),
              !availability.ready || availability.missingRoles.isEmpty else {
            throw SimulatorError.invalidSession("固件目录包含无效的设备或可用状态")
        }
    }
}

nonisolated struct BundledFirmwareCatalog: Decodable, Sendable {
    struct Runtime: Decodable, Sendable { let ready: Bool; let reason: String }
    let version: Int
    let items: [BundledFirmwareItem]
    let runtime: Runtime

    static func decode(_ data: Data) throws -> Self {
        let value: Self = try FirmwareLibraryValidation.decode(data)
        // Each imported full OTA is listed once per hardware preset.
        guard value.version == 1, value.items.count <= 4_096,
              Set(value.items.map(\.id)).count == value.items.count,
              FirmwareLibraryValidation.text(value.runtime.reason, limit: 1_024, empty: true) else {
            throw SimulatorError.invalidSession("固件目录版本或内容无效")
        }
        try value.items.forEach { try $0.validate() }
        return value
    }
}

nonisolated struct FirmwareImportReceipt: Decodable, Sendable {
    enum InputKind: String, Decodable, Sendable { case ap, ota }
    let version: Int
    let recognized: Bool
    let imported: Bool
    let inputKind: InputKind
    let sha256: String
    let needsSelection: Bool
    let candidateIDs: [String]
    let items: [BundledFirmwareItem]

    static func decode(_ data: Data) throws -> Self {
        let value: Self = try FirmwareLibraryValidation.decode(data)
        guard value.version == 1, value.recognized, value.imported,
              FirmwareLibraryValidation.digest(value.sha256),
              !value.candidateIDs.isEmpty, value.candidateIDs.count <= 256,
              Set(value.candidateIDs).count == value.candidateIDs.count,
              value.candidateIDs.allSatisfy(FirmwareLibraryValidation.identifier),
              value.items.count <= 256,
              Set(value.items.map(\.id)).count == value.items.count,
              Set(value.candidateIDs).isSubset(of: Set(value.items.map(\.id))),
              value.needsSelection || value.candidateIDs.count == 1 else {
            throw SimulatorError.invalidSession("导入结果没有明确对应的固件型号")
        }
        try value.items.forEach { try $0.validate() }
        return value
    }
}

/// A static description of the selected bytes, never a run authorization or a
/// catalog entry. Even a recognized boot header remains runtimeReady == false.
nonisolated struct FirmwareDiagnostic: Decodable, Sendable {
    enum InputKind: String, Decodable, Sendable {
        case apCandidate = "ap-candidate", fullOTA = "full-ota-zip"
    }
    struct AP: Decodable, Sendable {
        let sha256: String
        let bytes: Int
        let chip: String
        let headerRecognized: Bool
        let build: [String: String]?
        let diagnostic: String?
        let apOffset: Int?
        let flashBytes: Int?
        let dataAlias: Int?
        let resetEntry: Int?
        let resetSP: Int?
    }
    struct Archive: Decodable, Sendable {
        let members: Int
        let expandedBytes: Int
        let resourceLayout: String
        let allMemberCRCChecked: Bool
    }
    let version: Int
    let sha256: String
    let bytes: Int
    let inputKind: InputKind
    let executed: Bool
    let modified: Bool
    let compatibility: String
    let guestDebuggingVerified: Bool
    let versionWhitelistUsed: Bool
    let runtimeReady: Bool
    let nextStep: String
    let ap: AP
    let archive: Archive?

    var summary: String {
        ap.headerRecognized ? "已识别启动头；运行兼容性尚未验证" : "已完成静态检查；芯片或启动布局尚未适配"
    }
    var details: String {
        var lines = ["输入：\(inputKind == .fullOTA ? "完整 OTA ZIP" : "AP 候选文件") · \(bytes) 字节",
                     "文件 SHA-256：\(sha256)", "芯片：\(ap.chip)"]
        if let version = ap.build?["VERSION"] { lines.append("头部版本字段：\(version)") }
        if let archive {
            lines.append("包内 \(archive.members) 个成员的 CRC 已校验；资源布局：\(archive.resourceLayout)")
        }
        if let offset = ap.apOffset, let flash = ap.flashBytes, let entry = ap.resetEntry, let stack = ap.resetSP {
            lines.append(String(format: "AP 偏移：0x%X · Flash：0x%X · 启动入口：0x%X · 初始栈：0x%X", offset, flash, entry, stack))
        }
        if let diagnostic = ap.diagnostic { lines.append(diagnostic) }
        lines += [nextStep, "仅静态读取；未修改原文件，未启动固件，未验证客体调试能力。"]
        return lines.joined(separator: "\n")
    }

    static func decode(_ data: Data) throws -> Self {
        let value: Self = try FirmwareLibraryValidation.decode(data)
        let ap = value.ap
        guard value.version == 1, FirmwareLibraryValidation.digest(value.sha256),
              (1...805_306_368).contains(value.bytes), value.compatibility == "unverified",
              !value.executed, !value.modified, !value.guestDebuggingVerified,
              !value.versionWhitelistUsed, !value.runtimeReady,
              FirmwareLibraryValidation.text(value.nextStep, limit: 1_024),
              FirmwareLibraryValidation.digest(ap.sha256), (0...67_108_864).contains(ap.bytes),
              FirmwareLibraryValidation.text(ap.chip, limit: 128),
              ap.diagnostic.map({ FirmwareLibraryValidation.text($0, limit: 1_024) }) ?? true,
              (ap.build?.count ?? 0) <= 128,
              ap.build?.allSatisfy({ FirmwareLibraryValidation.identifier($0.key) && FirmwareLibraryValidation.text($0.value, limit: 2_048) }) ?? true else {
            throw SimulatorError.invalidSession("固件静态诊断内容无效；未赋予运行能力")
        }
        if ap.headerRecognized {
            guard ap.chip == "best1503", let offset = ap.apOffset, let flash = ap.flashBytes,
                  let alias = ap.dataAlias, let entry = ap.resetEntry, let stack = ap.resetSP,
                  0 <= offset, offset < flash, offset % 4096 == 0, flash <= 67_108_864,
                  ap.bytes <= flash - offset, (0x28000000..<0x30000000).contains(alias),
                  (1...0xffffffff).contains(entry), entry & 1 == 1,
                  (0x20000000...0x20160000).contains(stack), stack % 8 == 0 else {
                throw SimulatorError.invalidSession("固件启动头诊断边界无效")
            }
        }
        if value.inputKind == .fullOTA {
            guard let archive = value.archive, (1...4096).contains(archive.members),
                  (ap.bytes...1_073_741_824).contains(archive.expandedBytes), archive.allMemberCRCChecked,
                  ["romfs", "multi-partition"].contains(archive.resourceLayout) else {
                throw SimulatorError.invalidSession("固件包静态诊断不完整")
            }
        } else {
            guard value.archive == nil, value.bytes == ap.bytes, value.sha256 == ap.sha256 else {
                throw SimulatorError.invalidSession("AP 静态诊断与输入身份不一致")
            }
        }
        return value
    }
}

nonisolated struct BundledPreparedSession: Sendable {
    let root: URL
    let profileID: String
    let identityDigest: String
    let runLimitSeconds: Int
    let summary: String
    let executionApproved: Bool
    let instanceStorage: SimulatorJSON?

    static func decode(_ data: Data, requestedRoot: URL, profileID: String) throws -> Self {
        struct Receipt: Decodable {
            let version: Int; let id: String; let directory: String; let profileID: String
            let width: Int; let height: Int; let identityDigest: String
            let maxRuntimeSeconds: Int; let summary: String
            let executionApproved: Bool; let approvalRequired: Bool
            let instanceStorage: SimulatorJSON?
        }
        let value: Receipt = try FirmwareLibraryValidation.decode(data)
        // standardizedFileURL turns an existing /private/tmp path into /tmp, while the
        // helper and sandbox use /private/tmp. Keep the requested spelling and compare
        // symlink-resolved forms.
        let expected = requestedRoot.appendingPathComponent("session", isDirectory: true)
        let sameDirectory = URL(fileURLWithPath: value.directory, isDirectory: true).resolvingSymlinksInPath().path
            == expected.resolvingSymlinksInPath().path
        let checks: [(String, Bool)] = [
            ("version", value.version == 1), ("id", UUID(uuidString: value.id) != nil),
            ("directory", sameDirectory), ("profileID", value.profileID == profileID),
            ("identifier", FirmwareLibraryValidation.identifier(profileID)),
            ("geometry", [(192, 490), (212, 520), (336, 480)].contains(where: { $0 == (value.width, value.height) })),
            ("digest", FirmwareLibraryValidation.digest(value.identityDigest)),
            ("runLimit", StandaloneRunLimits.isValid(value.maxRuntimeSeconds)),
            ("summary", FirmwareLibraryValidation.text(value.summary, limit: 48_000)),
            ("approval", value.executionApproved != value.approvalRequired)]
        let failed = checks.filter { !$0.1 }.map(\.0)
        guard failed.isEmpty else {
            throw SimulatorError.invalidSession("准备结果与所选型号、会话目录或运行身份不一致（" + failed.joined(separator: ", ") + "）")
        }
        return Self(root: expected, profileID: profileID, identityDigest: value.identityDigest,
                    runLimitSeconds: value.maxRuntimeSeconds, summary: value.summary,
                    executionApproved: value.executionApproved, instanceStorage: value.instanceStorage)
    }
}

nonisolated enum FirmwareLibraryValidation {
    static let maximumResponseBytes = 4_194_304
    static func text(_ value: String, limit: Int, empty: Bool = false) -> Bool {
        (empty || !value.isEmpty) && value.utf8.count <= limit && !value.contains("\0")
    }
    static func identifier(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 128 && value.utf8.allSatisfy {
            (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [45, 46, 64, 95].contains($0)
        } && value != "." && value != ".."
    }
    static func digest(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    static func decode<T: Decodable>(_ data: Data) throws -> T {
        guard !data.isEmpty, data.count <= maximumResponseBytes else { throw SimulatorError.invalidSession("目录响应超出限制") }
        do { return try JSONDecoder().decode(T.self, from: data) }
        catch { throw SimulatorError.invalidSession("目录响应格式无效") }
    }
}

/// Injection never changes the production command set. Tests supply authored
/// JSON and record argv; the default runner resolves only packaged resources.
nonisolated struct BundledFirmwareService: Sendable {
    let run: @Sendable ([String]) async throws -> Data
    let cancelAll: @Sendable () async -> Void
    init() {
        let executor = StandaloneInspectionExecutor()
        run = { try await executor.run($0) }
        cancelAll = { await executor.cancelAll() }
    }
    init(run: @escaping @Sendable ([String]) async throws -> Data,
         cancelAll: @escaping @Sendable () async -> Void = {}) {
        self.run = run
        self.cancelAll = cancelAll
    }

    static var libraryRoot: URL {
        SimulatorDataLocations.firmwareRoot
    }
}

/// Validate the packaged paths, file attributes and dependency closure without
/// rehashing executable contents. macOS enforces the executable signature chain;
/// manifest digests remain release metadata, not a second startup allowlist.
nonisolated struct StandaloneBundledRuntime: Sendable {
    let engine: URL
    let python: URL
    let script: URL

    static func resolve(resources explicitResources: URL? = nil) throws -> Self {
        guard let resources = explicitResources ?? Bundle.main.resourceURL else {
            throw SimulatorError.transport("此应用未包含完整运行组件，请使用完整安装包")
        }
        let engine = resources.appendingPathComponent("Engine", isDirectory: true)
        let manifestURL = engine.appendingPathComponent("manifest.json")
        let manifestData: Data
        do { manifestData = try readRegular(manifestURL, limit: 2_097_152) }
        catch { throw SimulatorError.transport("此应用未包含可验证的内置运行组件；不会使用系统 Python 或外部环境") }
        struct Manifest: Decodable {
            struct File: Decodable { let path: String; let bytes: Int; let sha256: String; let executable: Bool }
            let version: Int
            let schema: String
            let entrypoints: [String: String]
            let files: [File]
        }
        let manifest: Manifest
        do { manifest = try JSONDecoder().decode(Manifest.self, from: manifestData) }
        catch { throw SimulatorError.transport("内置运行组件清单无效") }
        guard manifest.version == 1, manifest.schema == "azki.simulator.engine.v1", !manifest.files.isEmpty, manifest.files.count <= 8_192,
              manifest.entrypoints["python"] == "python/bin/python3.12",
              manifest.entrypoints["bootstrap"] == "bootstrap.py",
              manifest.entrypoints["project"] == "project",
              manifest.entrypoints["qemu"] == "project/backend/build/qemu-system-arm" else {
            throw SimulatorError.transport("内置运行组件版本或入口不匹配")
        }
        var listed = Set<String>()
        var listedDirectories = Set<String>()
        var total = 0
        for file in manifest.files {
            try Task.checkCancellation()
            guard safeRelativePath(file.path), listed.insert(file.path).inserted,
                  file.path != "manifest.json", (0...268_435_456).contains(file.bytes),
                  FirmwareLibraryValidation.digest(file.sha256) else {
                throw SimulatorError.transport("内置运行组件文件清单无效")
            }
            let components = file.path.split(separator: "/")
            if components.count > 1 {
                for count in 1..<components.count {
                    listedDirectories.insert(components.prefix(count).joined(separator: "/"))
                }
            }
            total += file.bytes
            guard total <= 1_073_741_824 else { throw SimulatorError.transport("内置运行组件超过检查限制") }
            let url = engine.appendingPathComponent(file.path)
            let fd = try openRegular(url, limit: file.bytes)
            defer { Darwin.close(fd) }
            var before = stat()
            guard fstat(fd, &before) == 0, before.st_size == file.bytes,
                  ((before.st_mode & 0o111) != 0) == file.executable else {
                throw SimulatorError.transport("内置运行组件文件属性不匹配")
            }
        }
        guard listed.contains("python/bin/python3.12"), listed.contains("bootstrap.py"),
              listed.contains("project/backend/build/qemu-system-arm") else {
            throw SimulatorError.transport("内置运行组件缺少必要文件")
        }
        // Unlisted files could alter Python imports or dynamic loading. Directory
        // traversal never follows links, and each listed file is opened atomically
        // with O_NOFOLLOW_ANY above; inherited writability is rejected as well.
        var enumerationFailed = false
        guard let entries = FileManager.default.enumerator(at: engine, includingPropertiesForKeys: nil,
            errorHandler: { _, _ in enumerationFailed = true; return false }) else {
            throw SimulatorError.transport("无法检查内置运行组件目录")
        }
        try validateDirectory(engine)
        var visited = 0
        let expectedCount = listed.count + listedDirectories.count + 1 // manifest.json
        for case let url as URL in entries {
            try Task.checkCancellation()
            visited += 1
            guard visited <= expectedCount else { throw SimulatorError.transport("内置运行组件目录超过清单范围") }
            var info = stat()
            guard lstat(url.path, &info) == 0 else { throw SimulatorError.transport("内置运行组件目录在检查时发生变化") }
            let kind = info.st_mode & S_IFMT
            let relative = String(url.path.dropFirst(engine.path.count + 1))
            if kind == S_IFDIR {
                guard listedDirectories.contains(relative) else { throw SimulatorError.transport("内置运行组件包含未登记的目录") }
                try validateDirectory(url)
            }
            else {
                guard kind == S_IFREG, relative == "manifest.json" || listed.contains(relative) else {
                    throw SimulatorError.transport("内置运行组件包含未登记的文件或链接")
                }
            }
        }
        guard !enumerationFailed, visited == expectedCount, try readRegular(manifestURL, limit: 2_097_152) == manifestData else {
            throw SimulatorError.transport("内置运行组件清单在检查时发生变化")
        }
        let script = resources.appendingPathComponent("Runtime/azki-simulator.py")
        _ = try readRegular(script, limit: 1_048_576)
        return Self(engine: engine, python: engine.appendingPathComponent("python/bin/python3.12"), script: script)
    }

    private static func safeRelativePath(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 1_024 && !value.hasPrefix("/") && !value.contains("\\") && !value.contains("\0")
            && value.split(separator: "/", omittingEmptySubsequences: false).count <= 32
            && value.split(separator: "/", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
    }
    private static func safeOwnerMode(_ info: stat) -> Bool {
        (info.st_uid == geteuid() || info.st_uid == 0) && info.st_mode & 0o022 == 0
    }
    private static func validateDirectory(_ url: URL) throws {
        let fd = Darwin.open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.transport("内置组件目录含链接或不可读") }
        defer { Darwin.close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, safeOwnerMode(info) else { throw SimulatorError.transport("内置组件目录权限不安全") }
    }
    private static func openRegular(_ url: URL, limit: Int) throws -> Int32 {
        // Opening a FIFO read-only can block before fstat can reject it. The
        // nonblocking flag has no effect on supported regular-file reads.
        let fd = Darwin.open(url.path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC)
        guard fd >= 0 else { throw SimulatorError.transport("内置组件文件缺失或包含链接") }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1,
              safeOwnerMode(info), info.st_size >= 0, info.st_size <= limit else {
            Darwin.close(fd)
            throw SimulatorError.transport("内置组件文件类型、权限或大小无效")
        }
        return fd
    }
    private static func readRegular(_ url: URL, limit: Int) throws -> Data {
        let fd = try openRegular(url, limit: limit)
        defer { Darwin.close(fd) }
        var before = stat(); guard fstat(fd, &before) == 0 else { throw SimulatorError.transport("组件读取失败") }
        var output = Data(), buffer = [UInt8](repeating: 0, count: 16_384)
        while true {
            try Task.checkCancellation()
            let n = Darwin.read(fd, &buffer, buffer.count)
            if n < 0 && errno == EINTR { continue }
            guard n >= 0 else { throw SimulatorError.transport("组件读取失败") }
            if n == 0 { break }
            output.append(contentsOf: buffer.prefix(n))
            guard output.count <= limit else { throw SimulatorError.transport("组件读取超出限制") }
        }
        var after = stat()
        guard fstat(fd, &after) == 0, stable(before, after), output.count == before.st_size else {
            throw SimulatorError.transport("内置组件在读取时发生变化")
        }
        return output
    }
    private static func stable(_ a: stat, _ b: stat) -> Bool {
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mode == b.st_mode
            && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec
            && a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec
    }
}
