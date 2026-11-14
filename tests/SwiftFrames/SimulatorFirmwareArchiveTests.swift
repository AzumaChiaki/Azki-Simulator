import XCTest
@testable import AzkiSimulator

final class SimulatorFirmwareArchiveTests: XCTestCase {
    func testArchiveImportWithoutInventedDisplayOrAP() throws {
        let digest = String(repeating: "a", count: 64)
        let id = "miwear.watch.l66@1.0.3@aaaaaaaaaaaa"
        let item: [String: Any] = ["id": id, "displayName": "旧设备增量包", "device": "miwear.watch.l66",
            "model": "miwear.watch.l66", "firmwareVersion": "1.0.3", "otaSHA256": digest,
            "width": 0, "height": 0, "source": "imported", "packageKind": "incremental",
            "container": "band-update", "baseVersion": "1.0.2", "metadataSource": "firmware",
            "availability": ["ready": false, "reason": "需要基础版本", "missingRoles": []]]
        let value: [String: Any] = ["version": 1, "recognized": true, "imported": true,
            "inputKind": "archive", "sha256": digest, "needsSelection": false,
            "candidateIDs": [id], "items": [item]]
        let receipt = try FirmwareImportReceipt.decode(JSONSerialization.data(withJSONObject: value))
        XCTAssertEqual(receipt.items.first?.baseVersion, "1.0.2")
        XCTAssertFalse(receipt.items.first!.availability.ready)
        var invalid = item
        invalid["availability"] = ["ready": true, "reason": "", "missingRoles": []] as [String: Any]
        let data = try JSONSerialization.data(withJSONObject: invalid)
        XCTAssertThrowsError(try JSONDecoder().decode(BundledFirmwareItem.self, from: data).validate())
    }

    func testOpaqueDiagnosticReportsItsActualInspectionScope() throws {
        let value: [String: Any] = ["version": 1, "sha256": String(repeating: "b", count: 64), "bytes": 1024,
            "inputKind": "unknown", "container": "raw-binary", "structure": "opaque",
            "firmwareVersion": "1.0.0", "declaredModels": ["mijia.watch.v1"], "metadataSource": "archive-filename",
            "executed": false, "modified": false, "compatibility": "unverified",
            "guestDebuggingVerified": false, "versionWhitelistUsed": false, "runtimeReady": false,
            "nextStep": "已导入原文件，内部封装尚未解析", "ap": NSNull()]
        let diagnostic = try FirmwareDiagnostic.decode(JSONSerialization.data(withJSONObject: value))
        XCTAssertNil(diagnostic.ap)
        XCTAssertTrue(diagnostic.summary.contains("内部尚未解析"))
        XCTAssertTrue(diagnostic.details.contains("mijia.watch.v1"))
    }
}
