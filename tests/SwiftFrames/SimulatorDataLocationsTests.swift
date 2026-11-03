import XCTest
import Foundation
import Darwin
@testable import AzkiSimulator

final class SimulatorDataLocationsTests: XCTestCase {
    private let developmentID = "com.azki.AzkiSimulator.development.q65-o62m-20261102"
    private let support = URL(fileURLWithPath: "/unused-user-support", isDirectory: true)

    private func withDirectory(_ body: (URL) throws -> Void) throws {
        // macOS's /var temporary-directory alias is intentionally rejected by
        // the production no-symlink walk. Keep fixtures in the isolated build.
        let build = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("build", isDirectory: true)
        try FileManager.default.createDirectory(at: build, withIntermediateDirectories: true)
        let root = build
            .appendingPathComponent("azki-data-locations-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false,
                                               attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: root) }
        try body(root)
    }

    private func resolve(_ path: String, identifier: String? = nil) throws -> SimulatorDataLocations.Locations {
        try SimulatorDataLocations.resolve(bundleIdentifier: identifier ?? developmentID,
            environment: [SimulatorDataLocations.developmentDataRootVariable: path], applicationSupport: support)
    }

    func testPrivateDevelopmentRootContainsAllLocationsWithoutCreatingFiles() throws {
        try withDirectory { root in
            let locations = try resolve(root.path + "/")
            XCTAssertEqual(locations.appSupportRoot, root)
            XCTAssertEqual(locations.instanceRoot, root.appendingPathComponent("Instances", isDirectory: true))
            XCTAssertEqual(locations.firmwareRoot, root.appendingPathComponent("FirmwareLibrary", isDirectory: true))
            XCTAssertTrue(locations.isolatedDevelopment)
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: root.path).isEmpty)
        }
    }

    func testProductionAndInvalidDevelopmentIdentitiesIgnoreOverride() throws {
        let invalidIDs: [String?] = [nil, "com.azki.AzkiSimulator", "com.azki.AzkiSimulator.development.",
            "com.azki.AzkiSimulator.development.UPPER", "com.azki.AzkiSimulator.development.a/b",
            "com.azki.AzkiSimulator.development.a.b", "com.azki.AzkiSimulator.development." + String(repeating: "x", count: 65)]
        for identifier in invalidIDs {
            let locations = try SimulatorDataLocations.resolve(bundleIdentifier: identifier,
                environment: [SimulatorDataLocations.developmentDataRootVariable: "/missing/invalid/override"],
                applicationSupport: support)
            XCTAssertEqual(locations.instanceRoot.path, "/unused-user-support/AzkiSimulator/Instances")
            XCTAssertEqual(locations.firmwareRoot.path, "/unused-user-support/com.azki.AzkiSimulator/FirmwareLibrary")
            XCTAssertEqual(locations.appSupportRoot.path, "/unused-user-support/com.azki.AzkiSimulator")
            XCTAssertFalse(locations.isolatedDevelopment)
        }
    }

    func testDevelopmentWithoutOverrideKeepsExistingNamespace() throws {
        let locations = try SimulatorDataLocations.resolve(bundleIdentifier: developmentID, environment: [:],
                                                           applicationSupport: support)
        XCTAssertEqual(locations.appSupportRoot.path, support.appendingPathComponent(developmentID).path)
        XCTAssertEqual(locations.instanceRoot.deletingLastPathComponent(), locations.appSupportRoot)
        XCTAssertEqual(locations.firmwareRoot.deletingLastPathComponent(), locations.appSupportRoot)
        XCTAssertFalse(locations.isolatedDevelopment)
    }

    func testGlobalCleanupIsolationRequiresValidatedDevelopmentIdentityAndExplicitPrivateRoot() throws {
        try withDirectory { root in
            let override = [SimulatorDataLocations.developmentDataRootVariable: root.path]
            let cases: [(String?, [String: String], Bool)] = [
                (developmentID, override, true),
                ("com.azki.AzkiSimulator", override, false),
                (nil, override, false),
                ("com.azki.AzkiSimulator.development.", override, false),
                ("com.azki.AzkiSimulator.development.UPPER", override, false),
                (developmentID, [:], false),
                (developmentID, ["AZKI_SIMULATOR_DATA_ROOT": root.path], false)
            ]
            for (identifier, environment, expected) in cases {
                let locations = try SimulatorDataLocations.resolve(bundleIdentifier: identifier,
                    environment: environment, applicationSupport: support)
                XCTAssertEqual(locations.isolatedDevelopment, expected,
                               "identity=\(identifier ?? "nil"), explicitOverride=\(environment.keys.sorted())")
            }
            // A syntactically valid identity and present variable cannot enable
            // the skip when the filesystem checks fail. No fallback is returned.
            XCTAssertEqual(chmod(root.path, 0o755), 0)
            XCTAssertThrowsError(try resolve(root.path)) { error in
                XCTAssertEqual(error as? SimulatorDataLocations.LocationError, .unsafePermissions)
            }
        }
    }

    func testInvalidOverrideThrowsWithoutFallbackOrDirectoryCreation() throws {
        try withDirectory { root in
            let missing = root.appendingPathComponent("missing")
            for path in ["", "/", "relative", root.path + "/../escape", root.path + "/./data",
                         root.path + "//data", root.path + "\0ignored", missing.path] {
                XCTAssertThrowsError(try resolve(path), path)
            }
            XCTAssertFalse(FileManager.default.fileExists(atPath: missing.path))
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: root.path).isEmpty)
        }
    }

    func testRejectsFilesAndSymlinksIncludingIntermediateComponents() throws {
        try withDirectory { root in
            let file = root.appendingPathComponent("file")
            try Data().write(to: file)
            XCTAssertEqual(chmod(file.path, 0o700), 0)
            XCTAssertThrowsError(try resolve(file.path))
            let child = root.appendingPathComponent("child", isDirectory: true)
            try FileManager.default.createDirectory(at: child, withIntermediateDirectories: false,
                                                   attributes: [.posixPermissions: 0o700])
            XCTAssertNoThrow(try resolve(child.path))
            let link = root.appendingPathComponent("linked-child")
            try FileManager.default.createSymbolicLink(at: link, withDestinationURL: child)
            XCTAssertThrowsError(try resolve(link.path))
            let parentLink = root.appendingPathComponent("linked-parent")
            try FileManager.default.createSymbolicLink(at: parentLink, withDestinationURL: root)
            XCTAssertThrowsError(try resolve(parentLink.appendingPathComponent("child").path))
        }
    }

    func testRequiresExactPrivatePermissionsWithoutRepairingThem() throws {
        try withDirectory { root in
            for mode: mode_t in [0o755, 0o770, 0o777, 0o1700] {
                XCTAssertEqual(chmod(root.path, mode), 0)
                XCTAssertThrowsError(try resolve(root.path)) { error in
                    XCTAssertEqual(error as? SimulatorDataLocations.LocationError, .unsafePermissions)
                }
                var info = stat()
                XCTAssertEqual(lstat(root.path, &info), 0)
                XCTAssertEqual(info.st_mode & 0o7777, mode)
            }
        }
    }

    func testRejectsDirectoryOwnedByAnotherUser() throws {
        var info = stat()
        guard lstat("/private", &info) == 0, info.st_uid != getuid() else {
            throw XCTSkip("Requires the standard root-owned macOS /private directory")
        }
        XCTAssertThrowsError(try resolve("/private")) { error in
            XCTAssertEqual(error as? SimulatorDataLocations.LocationError, .wrongOwner)
        }
    }
}
