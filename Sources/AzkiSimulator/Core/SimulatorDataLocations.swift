import Foundation
import Darwin

/// Signed local development copies can use their own bundle identity without
/// sharing instance metadata or accepted guest revisions with the user's app.
/// The production identifier keeps the existing on-disk locations unchanged.
nonisolated enum SimulatorDataLocations {
    static let developmentDataRootVariable = "AZKI_SIMULATOR_DEVELOPMENT_DATA_ROOT"

    struct Locations: Equatable, Sendable {
        let instanceRoot: URL
        let firmwareRoot: URL
        let appSupportRoot: URL
        /// True only after both the development identity and explicit private
        /// root have passed validation. A development bundle alone is not enough.
        let isolatedDevelopment: Bool
    }

    enum LocationError: LocalizedError, Equatable {
        case invalidPath
        case inaccessibleDirectory(Int32)
        case wrongOwner
        case unsafePermissions

        var errorDescription: String? {
            switch self {
            case .invalidPath: "开发数据目录必须是绝对路径，不能包含空路径段、.、.. 或 NUL。"
            case .inaccessibleDirectory(let code): "开发数据目录必须已存在且不包含符号链接（errno \(code)）。"
            case .wrongOwner: "开发数据目录必须属于当前用户。"
            case .unsafePermissions: "开发数据目录权限必须为 0700。"
            }
        }
    }

    static func developmentNamespace(bundleIdentifier: String?) -> String? {
        let prefix = "com.azki.AzkiSimulator.development."
        guard let bundleIdentifier, bundleIdentifier.hasPrefix(prefix) else { return nil }
        let suffix = bundleIdentifier.dropFirst(prefix.count)
        guard !suffix.isEmpty, suffix.count <= 64,
              suffix.utf8.allSatisfy({ (97...122).contains($0) || (48...57).contains($0) || $0 == 45 }) else { return nil }
        return bundleIdentifier
    }

    static var instanceRoot: URL { selected.instanceRoot }
    static var firmwareRoot: URL { selected.firmwareRoot }
    static var isolatedDevelopment: Bool { selected.isolatedDevelopment }

    /// The whole app storage directory (instances, firmware library, run
    /// snapshots). The development namespace keeps a signed copy separate.
    static var appSupportRoot: URL { selected.appSupportRoot }

    /// Resolve once for all consumers. An invalid explicit development root
    /// must stop startup, never fall back to the user's normal data directory.
    private static let selected: Locations = {
        do {
            return try resolve(bundleIdentifier: Bundle.main.bundleIdentifier,
                               environment: ProcessInfo.processInfo.environment,
                               applicationSupport: FileManager.default.urls(for: .applicationSupportDirectory,
                                                                           in: .userDomainMask)[0])
        } catch {
            fatalError("\(developmentDataRootVariable): \(error.localizedDescription)")
        }
    }()

    static func resolve(bundleIdentifier: String?, environment: [String: String],
                        applicationSupport: URL) throws -> Locations {
        let namespace = developmentNamespace(bundleIdentifier: bundleIdentifier)
        if namespace != nil, let path = environment[developmentDataRootVariable] {
            let root = try checkedDevelopmentDirectory(path)
            return Locations(instanceRoot: root.appendingPathComponent("Instances", isDirectory: true),
                             firmwareRoot: root.appendingPathComponent("FirmwareLibrary", isDirectory: true),
                             appSupportRoot: root,
                             isolatedDevelopment: true)
        }
        let root = applicationSupport.appendingPathComponent(namespace ?? "com.azki.AzkiSimulator", isDirectory: true)
        return Locations(
            instanceRoot: applicationSupport.appendingPathComponent(namespace.map { $0 + "/Instances" }
                                                                    ?? "AzkiSimulator/Instances", isDirectory: true),
            firmwareRoot: root.appendingPathComponent("FirmwareLibrary", isDirectory: true),
            appSupportRoot: root,
            isolatedDevelopment: false)
    }

    private static func checkedDevelopmentDirectory(_ path: String) throws -> URL {
        guard path.hasPrefix("/"), !path.contains("\0"), path.utf8.count < Int(PATH_MAX) else {
            throw LocationError.invalidPath
        }
        var normalized = path
        while normalized.hasSuffix("/") { normalized.removeLast() }
        let components = normalized.split(separator: "/", omittingEmptySubsequences: false).dropFirst()
        guard !components.isEmpty, components.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
            throw LocationError.invalidPath
        }
        // Walk from / with no-follow directory FDs: checking only the final
        // component would still permit an intermediate symlink to escape.
        let flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
        var descriptor = Darwin.open("/", flags)
        guard descriptor >= 0 else { throw LocationError.inaccessibleDirectory(errno) }
        defer { Darwin.close(descriptor) }
        for component in components {
            let next = String(component).withCString { openat(descriptor, $0, flags) }
            guard next >= 0 else { throw LocationError.inaccessibleDirectory(errno) }
            Darwin.close(descriptor)
            descriptor = next
        }
        var info = stat()
        guard fstat(descriptor, &info) == 0 else { throw LocationError.inaccessibleDirectory(errno) }
        guard info.st_uid == getuid() else { throw LocationError.wrongOwner }
        guard info.st_mode & 0o7777 == 0o700 else { throw LocationError.unsafePermissions }
        return URL(fileURLWithPath: normalized, isDirectory: true)
    }
}
