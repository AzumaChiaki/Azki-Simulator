import XCTest
import Foundation
import CoreGraphics
import ImageIO
@testable import AzkiSimulator

@MainActor
final class SimulatorInteractionLogicTests: XCTestCase {
    private func makeInstance(isServing: Bool = false) -> SimulatorInstanceContext {
        let record = SimulatorInstanceRecord(
            id: UUID(),
            name: "测试手环",
            model: "miwear.watch.q66",
            selectedProfileID: nil,
            createdAt: Date(),
            virtualIdentity: nil,
            skipOOBE: true
        )
        let context = SimulatorInstanceContext(record: record)
        if isServing {
            let helper = Process()
            helper.executableURL = URL(fileURLWithPath: "/usr/bin/true")
            // Use a fresh serving runtime directly when testing serving states.
            let runtime = StandaloneRuntimeModel(ownedHelper: helper, modelConstraint: record.model, instanceID: record.id)
            _ = runtime
        }
        return context
    }

    func testRunAndImportRemainAvailableWithoutExcessivePreconditions() {
        let instance = makeInstance()
        XCTAssertTrue(SimulatorInstanceActions.canRun(instance))
        XCTAssertTrue(SimulatorInstanceActions.importFirmware(instance))
        XCTAssertTrue(SimulatorInstanceActions.controlsAvailable(instance))
        XCTAssertTrue(SimulatorWorkspaceView.canEnableSharing(runtime: instance.runtime, sharing: instance.sharing))
        XCTAssertTrue(SimulatorWorkspaceView.coreControlsAvailable(runtime: instance.runtime, sharing: instance.sharing))
    }

    func testServingRuntimeStillAllowsRunImportAndCoreControls() {
        let helper = Process()
        let runtime = StandaloneRuntimeModel(ownedHelper: helper, modelConstraint: "miwear.watch.q66", instanceID: UUID())
        let sharing = HostStatusSharingModel()
        XCTAssertTrue(runtime.isServing)
        XCTAssertTrue(SimulatorWorkspaceView.coreControlsAvailable(runtime: runtime, sharing: sharing))
        XCTAssertTrue(SimulatorWorkspaceView.canEnableSharing(runtime: runtime, sharing: sharing))
    }

    func testToastLifecycleShowsAndDismissesCleanly() {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("azki-toast-\(UUID().uuidString)")
        let library = SimulatorInstanceLibrary(store: SimulatorInstanceRecordStore(rootURL: root))
        XCTAssertNil(library.toastMessage)
        library.showToast("已自动选择固件", isError: false, durationSeconds: 60)
        XCTAssertEqual(library.toastMessage, "已自动选择固件")
        XCTAssertFalse(library.toastIsError)
        library.dismissToast()
        XCTAssertNil(library.toastMessage)
    }

    func testCanCreateDoesNotBlockOnEmptyCatalogWhileStorageAvailable() throws {
        let root = URL(fileURLWithPath: "/private/tmp", isDirectory: true)
            .appendingPathComponent("azki-create-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: root) }
        let library = SimulatorInstanceLibrary(store: SimulatorInstanceRecordStore(rootURL: root))
        XCTAssertTrue(library.storageAvailable)
        XCTAssertTrue(library.deviceModels.isEmpty)
        XCTAssertNil(SimulatorInstanceActions.canCreateReason(library))
    }

    func testGuestOperationsCanStartInstallFlowWithoutPriorAuth() {
        let model = SimulatorGuestOperationsModel()
        XCTAssertFalse(model.canStartInstallFlow)
        model.configure(
            hostSessionID: UUID(),
            available: true,
            packagesAvailable: true,
            command: { _, _ in .object([:]) },
            install: { _, _, _ in .object([:]) }
        )
        XCTAssertTrue(model.canStartInstallFlow)
        model.invalidate()
        XCTAssertFalse(model.canStartInstallFlow)
    }

    func testDisplaySettingsDefaultsEnabledAndPersistsToggle() {
        let suiteName = "azki.display.test.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let settings = DisplaySettings(defaults: defaults)
        XCTAssertTrue(settings.clipToScreenShape)
        settings.clipToScreenShape = false
        XCTAssertFalse(DisplaySettings.readClipToScreenShape(from: defaults))
        settings.clipToScreenShape = true
        XCTAssertTrue(DisplaySettings.readClipToScreenShape(from: defaults))
    }

    func testScreenShapeMaskClipsCornersForBandAndWatchAndProducesTransparentPNG() throws {
        // Test capsule (212x520) and round watch (466x466) corner containment
        let bandRect = CGRect(x: 0, y: 0, width: 212, height: 520)
        XCTAssertFalse(SimulatorDisplayGeometry.pixels212x520.containsInScreenShape(CGPoint(x: 2, y: 2), in: bandRect))
        XCTAssertTrue(SimulatorDisplayGeometry.pixels212x520.containsInScreenShape(CGPoint(x: 106, y: 260), in: bandRect))

        let watchRect = CGRect(x: 0, y: 0, width: 466, height: 466)
        XCTAssertFalse(SimulatorDisplayGeometry.pixels466x466.containsInScreenShape(CGPoint(x: 5, y: 5), in: watchRect))
        XCTAssertTrue(SimulatorDisplayGeometry.pixels466x466.containsInScreenShape(CGPoint(x: 233, y: 233), in: watchRect))

        // Build a solid white 212x520 source CGImage and verify clipped PNG has alpha=0 at (0,0) and alpha=255 at center.
        let width = 212, height = 520
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        let ctx = CGContext(
            data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
        )!
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let source = ctx.makeImage()!

        let outDir = URL(fileURLWithPath: "/private/tmp", isDirectory: true)
            .appendingPathComponent("azki-shot-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: outDir) }

        let clippedURL = try SimulatorScreenshot.save(source, instanceName: "Band11",
                                                      clipGeometry: .pixels212x520, directory: outDir)
        let imgSource = CGImageSourceCreateWithURL(clippedURL as CFURL, nil)!
        let loaded = CGImageSourceCreateImageAtIndex(imgSource, 0, nil)!
        XCTAssertEqual(loaded.width, width)
        XCTAssertEqual(loaded.height, height)

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let readCtx = CGContext(
            data: &pixels, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
        )!
        readCtx.draw(loaded, in: CGRect(x: 0, y: 0, width: width, height: height))
        // Corner pixel (0, 0) must be fully transparent
        XCTAssertEqual(pixels[3], 0)
        // Center pixel (106, 260) must be fully opaque white
        let centerOffset = (260 * width + 106) * 4
        XCTAssertEqual(pixels[centerOffset + 0], 255)
        XCTAssertEqual(pixels[centerOffset + 3], 255)
    }

    func testClippedRecordingMasksOuterCornersToBlackAcrossAllGeometries() throws {
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        for geometry in SimulatorDisplayGeometry.allCases {
            let width = geometry.width, height = geometry.height
            let srcCtx = CGContext(
                data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
            )!
            srcCtx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
            srcCtx.fill(CGRect(x: 0, y: 0, width: width, height: height))
            let whiteFrame = srcCtx.makeImage()!

            let masked = try XCTUnwrap(SimulatorDisplayGeometry.maskedCGImage(
                whiteFrame, geometry: geometry, transparentBackground: false))
            var pixels = [UInt8](repeating: 0, count: width * height * 4)
            let readCtx = CGContext(
                data: &pixels, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
            )!
            readCtx.draw(masked, in: CGRect(x: 0, y: 0, width: width, height: height))
            // Top-left corner (0, 0) outside the physical screen silhouette must be pure black
            XCTAssertEqual(pixels[0], 0, "corner R for \(geometry)")
            XCTAssertEqual(pixels[1], 0, "corner G for \(geometry)")
            XCTAssertEqual(pixels[2], 0, "corner B for \(geometry)")
            // Center pixel must remain pure white
            let center = ((height / 2) * width + (width / 2)) * 4
            XCTAssertEqual(pixels[center + 0], 255, "center R for \(geometry)")
            XCTAssertEqual(pixels[center + 1], 255, "center G for \(geometry)")
            XCTAssertEqual(pixels[center + 2], 255, "center B for \(geometry)")
        }
    }
}
