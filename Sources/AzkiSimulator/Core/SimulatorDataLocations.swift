import Foundation

/// Signed local development copies can use their own bundle identity without
/// sharing instance metadata or accepted guest revisions with the user's app.
/// The production identifier keeps the existing on-disk locations unchanged.
nonisolated enum SimulatorDataLocations {
    static func developmentNamespace(bundleIdentifier: String?) -> String? {
        let prefix = "com.azki.AzkiSimulator.development."
        guard let bundleIdentifier, bundleIdentifier.hasPrefix(prefix) else { return nil }
        let suffix = bundleIdentifier.dropFirst(prefix.count)
        guard !suffix.isEmpty, suffix.count <= 64,
              suffix.utf8.allSatisfy({ (97...122).contains($0) || (48...57).contains($0) || $0 == 45 }) else { return nil }
        return bundleIdentifier
    }

    static var instanceRoot: URL {
        support.appendingPathComponent(namespace.map { $0 + "/Instances" } ?? "AzkiSimulator/Instances", isDirectory: true)
    }

    static var firmwareRoot: URL {
        support.appendingPathComponent((namespace ?? "com.azki.AzkiSimulator") + "/FirmwareLibrary", isDirectory: true)
    }

    /// The whole app storage directory (instances, firmware library, run
    /// snapshots). The development namespace keeps a signed copy separate.
    static var appSupportRoot: URL {
        support.appendingPathComponent(namespace ?? "com.azki.AzkiSimulator", isDirectory: true)
    }

    private static var namespace: String? { developmentNamespace(bundleIdentifier: Bundle.main.bundleIdentifier) }
    private static var support: URL { FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0] }
}
