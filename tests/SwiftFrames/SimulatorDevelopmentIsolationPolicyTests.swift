import XCTest
import Foundation
import Darwin
@testable import AzkiSimulator

/// Builds production inspection policies only. No interpreter, app, guest,
/// cleanup action or process is launched by these tests.
final class SimulatorDevelopmentIsolationPolicyTests: XCTestCase {
    private let identifier = "7ee1a6b4-d45c-4b57-9a43-66f9be2e7bdd"

    private struct Fixture {
        let root: URL
        let library: URL
        let temporary: URL
        let runtime: StandaloneBundledRuntime
    }

    private func withFixture(_ body: (Fixture) throws -> Void) throws {
        let build = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("build", isDirectory: true)
        try FileManager.default.createDirectory(at: build, withIntermediateDirectories: true)
        let root = build.appendingPathComponent("isolation-policy-" + UUID().uuidString, isDirectory: true)
        try makeDirectory(root)
        // Retain these small owned fixtures as evidence; never clean /private/tmp.
        let library = root.appendingPathComponent("FirmwareLibrary", isDirectory: true)
        let temporary = root.appendingPathComponent("command", isDirectory: true)
        let engine = root.appendingPathComponent("Engine", isDirectory: true)
        let scripts = root.appendingPathComponent("Runtime", isDirectory: true)
        for directory in [library, temporary, engine, scripts] { try makeDirectory(directory) }
        let runtime = StandaloneBundledRuntime(engine: engine,
            python: engine.appendingPathComponent("python-unused"),
            script: scripts.appendingPathComponent("azki-simulator-unused.py"))
        try body(Fixture(root: root, library: library, temporary: temporary, runtime: runtime))
    }

    private func makeDirectory(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false,
                                               attributes: [.posixPermissions: 0o700])
    }

    private func instance(_ fixture: Fixture, storage: Bool = true) throws -> URL {
        let container = fixture.library.appendingPathComponent("instances", isDirectory: true)
        try makeDirectory(container)
        let instance = container.appendingPathComponent(identifier, isDirectory: true)
        try makeDirectory(instance)
        if storage { try makeDirectory(instance.appendingPathComponent("storage", isDirectory: true)) }
        return instance
    }

    private func inspection(_ arguments: [String], _ fixture: Fixture, isolated: Bool) throws -> StandaloneSandboxLaunch {
        try StandaloneProcessSandbox.makeInspection(runtime: fixture.runtime, arguments: arguments,
            library: fixture.library, temporary: fixture.temporary, isolatedDevelopment: isolated)
    }

    private func attributes(_ url: URL) throws -> [String: String] {
        var info = stat()
        guard lstat(url.path, &info) == 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        return ["inode": String(info.st_ino), "device": String(info.st_dev), "mode": String(info.st_mode),
                "size": String(info.st_size), "mtime": "\(info.st_mtimespec.tv_sec):\(info.st_mtimespec.tv_nsec)",
                "ctime": "\(info.st_ctimespec.tv_sec):\(info.st_ctimespec.tv_nsec)"]
    }

    func testIsolatedModeRejectsEveryGlobalScanIncludingScopedAndDryRunMaintenance() throws {
        try withFixture { fixture in
            let commands = [["sessions-cleanup"], ["storage-report"], ["storage-maintain", "--auto"],
                ["storage-maintain", "--keep", "2", "--instance-id", identifier],
                ["storage-maintain", "--auto", "--dry-run"], ["instance-delete", "--instance-id", identifier]]
            let before = try attributes(fixture.library)
            for arguments in commands {
                XCTAssertThrowsError(try inspection(arguments, fixture, isolated: true), arguments.joined(separator: " ")) { error in
                    XCTAssertTrue(error.localizedDescription.contains("隔离开发模式不允许扫描或清理全局临时会话"))
                }
            }
            XCTAssertEqual(try attributes(fixture.library), before)
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: fixture.library.path).isEmpty)
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: fixture.temporary.path).isEmpty)
        }
    }

    func testProductionModeRetainsExistingGlobalPolicies() throws {
        try withFixture { fixture in
            let commands = [["sessions-cleanup"], ["storage-maintain", "--auto"],
                            ["instance-delete", "--instance-id", identifier]]
            for arguments in commands {
                let launch = try inspection(arguments, fixture, isolated: false)
                for rule in StandaloneProcessSandbox.sessionCleanupRules { XCTAssertTrue(launch.policyText.contains(rule)) }
            }
            let report = try inspection(["storage-report"], fixture, isolated: false)
            for rule in StandaloneProcessSandbox.storageReadRules { XCTAssertTrue(report.policyText.contains(rule)) }
        }
    }

    func testIsolatedResetWithoutPendingHasOnlyBoundedWrites() throws {
        try withFixture { fixture in
            let target = try instance(fixture)
            let arguments = ["instance-reset", "--instance-id", identifier]
            let isolated = try inspection(arguments, fixture, isolated: true)
            XCTAssertTrue(isolated.policyText.contains("(allow file-read* file-write* (subpath \"" + target.path + "\"))"))
            XCTAssertFalse(isolated.policyText.contains("(regex #\"^/private/tmp/azki-"))
            XCTAssertFalse(isolated.policyText.contains("(allow file-read* (literal \"/private/tmp\"))"))
            let production = try inspection(arguments, fixture, isolated: false)
            for rule in StandaloneProcessSandbox.preparedSessionRules { XCTAssertTrue(production.policyText.contains(rule)) }
        }
    }

    func testPendingRejectsResetAndEmbeddedPrepareBeforeReservationOrRollback() throws {
        try withFixture { fixture in
            let target = try instance(fixture)
            let pending = target.appendingPathComponent("storage/pending.json")
            let data = Data("{\"sessionRoot\":\"/private/tmp/azki-b-unowned-fixture/session\",\"revision\":\"unchanged\"}".utf8)
            _ = try StandaloneSandboxFiles.writeExclusive(data, to: pending)
            let before = try attributes(pending)
            let reserved = URL(fileURLWithPath: "/private/tmp/azki-b-reject-" + UUID().uuidString.lowercased().prefix(12))
            XCTAssertFalse(FileManager.default.fileExists(atPath: reserved.path))
            let commands = [["instance-reset", "--instance-id", identifier],
                ["prepare-bundled", "--profile-id", "q65", "--session-root", reserved.path, "--instance-id", identifier]]
            for arguments in commands {
                XCTAssertThrowsError(try inspection(arguments, fixture, isolated: true)) { error in
                    XCTAssertTrue(error.localizedDescription.contains("待处理会话"))
                }
                XCTAssertEqual(try Data(contentsOf: pending), data)
                XCTAssertEqual(try attributes(pending), before)
                XCTAssertFalse(FileManager.default.fileExists(atPath: reserved.path))
            }
            // Production keeps its old recovery policy; no helper is executed.
            XCTAssertNoThrow(try inspection(commands[0], fixture, isolated: false))
        }
    }

    func testPendingSymlinkAndDirectoryFailClosedWithoutFollowingOrRemovingThem() throws {
        for kind in ["symlink", "directory"] {
            try withFixture { fixture in
                let target = try instance(fixture)
                let pending = target.appendingPathComponent("storage/pending.json")
                let missing = fixture.root.appendingPathComponent("must-not-create")
                if kind == "symlink" { try FileManager.default.createSymbolicLink(at: pending, withDestinationURL: missing) }
                else { try makeDirectory(pending) }
                let before = try attributes(pending)
                XCTAssertThrowsError(try inspection(["instance-reset", "--instance-id", identifier], fixture, isolated: true))
                XCTAssertEqual(try attributes(pending), before)
                XCTAssertFalse(FileManager.default.fileExists(atPath: missing.path))
            }
        }
    }

    func testStorageSymlinkAndNonDirectoryFailClosed() throws {
        for kind in ["symlink", "file"] {
            try withFixture { fixture in
                let target = try instance(fixture, storage: false)
                let storage = target.appendingPathComponent("storage")
                let external = fixture.root.appendingPathComponent("other-instance", isDirectory: true)
                try makeDirectory(external)
                if kind == "symlink" { try FileManager.default.createSymbolicLink(at: storage, withDestinationURL: external) }
                else { _ = try StandaloneSandboxFiles.writeExclusive(Data("not a directory".utf8), to: storage) }
                let before = try attributes(storage)
                XCTAssertThrowsError(try inspection(["instance-reset", "--instance-id", identifier], fixture, isolated: true))
                XCTAssertEqual(try attributes(storage), before)
                XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: external.path).isEmpty)
            }
        }
    }

    func testMissingStorageIsAllowedWithoutCreatingIt() throws {
        try withFixture { fixture in
            let target = try instance(fixture, storage: false)
            XCTAssertNoThrow(try inspection(["instance-reset", "--instance-id", identifier], fixture, isolated: true))
            XCTAssertFalse(FileManager.default.fileExists(atPath: target.appendingPathComponent("storage").path))
        }
    }
}
