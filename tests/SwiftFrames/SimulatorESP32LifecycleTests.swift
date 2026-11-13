import XCTest
@testable import AzkiSimulator

final class SimulatorESP32LifecycleTests: XCTestCase {
    func testGeneratedSessionDirectoryIsAcceptedByPreparePolicy() throws {
        let directory = ESP32BridgeSession.newSessionDirectory()
        try StandaloneSandboxFiles.createPrivate(directory)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ready = directory.appendingPathComponent("ready.json")
        let socket = directory.appendingPathComponent("hci.sock")
        let data = try JSONSerialization.data(withJSONObject: ["ready": true,
            "port": "/dev/cu.usbserial-test", "hciSocket": socket.path])
        _ = try StandaloneSandboxFiles.writeExclusive(data, to: ready)
        let accepted = try StandaloneProcessSandbox.physicalBluetoothReady(ready.path)
        XCTAssertEqual(accepted.directory.path, directory.path)
        XCTAssertEqual(accepted.socket, socket)
    }

    func testControllerErrorCannotBecomeSuccessfulCleanup() {
        var receipt: [String: Any] = ["closed": true, "stopConfirmed": true,
            "leaseReleased": true, "cleanupComplete": true, "firstError": NSNull(),
            "portOpened": true, "controllerStarted": true]
        XCTAssertTrue(ESP32BridgeSession.cleanupConfirmed(receipt))
        receipt["firstError"] = "native I/O failed"
        XCTAssertFalse(ESP32BridgeSession.cleanupConfirmed(receipt))
        receipt["portOpened"] = false
        XCTAssertFalse(ESP32BridgeSession.cleanupConfirmed(receipt))
        receipt["controllerStarted"] = false
        XCTAssertTrue(ESP32BridgeSession.cleanupConfirmed(receipt))
        receipt["leaseReleased"] = false
        XCTAssertFalse(ESP32BridgeSession.cleanupConfirmed(receipt))
        XCTAssertFalse(ESP32BridgeSession.cleanupConfirmed([:]))
    }

    func testStoppedProcessWithoutHardwareStopReceiptIsNotEnough() {
        let receipt: [String: Any] = ["closed": true, "leaseReleased": true,
            "cleanupComplete": true, "firstError": NSNull(), "exitCode": 0]
        XCTAssertFalse(ESP32BridgeSession.cleanupConfirmed(receipt))
    }

    func testSelectedSerialPortStaysAnExactDevicePath() throws {
        try SimulatorSerialDevice.validatePath("/dev/cu.usbmodem101")
        XCTAssertThrowsError(try SimulatorSerialDevice.validatePath("/dev/cu.usb/other"))
        XCTAssertThrowsError(try SimulatorSerialDevice.validatePath("/tmp/cu.usbmodem101"))
        XCTAssertThrowsError(try SimulatorSerialDevice.validatePath(""))
    }
}

private actor StorageFinishGate {
    private var continuation: CheckedContinuation<Data, Error>?
    private var entered: CheckedContinuation<Void, Never>?
    private var calls = 0
    private let receipt = Data(#"{"committed":true,"norPersistent":true}"#.utf8)

    func run(_ arguments: [String]) async throws -> Data {
        guard arguments.first == "instance-storage-finish" else { throw SimulatorError.transport("unexpected command") }
        calls += 1
        if calls > 1 { return receipt }
        return try await withCheckedThrowingContinuation { continuation in
            self.continuation = continuation
            entered?.resume(); entered = nil
        }
    }
    func waitUntilEntered() async {
        if continuation != nil { return }
        await withCheckedContinuation { entered = $0 }
    }
    func count() -> Int { calls }
    func release(fail: Bool = false) {
        let pending = continuation; continuation = nil
        if fail { pending?.resume(throwing: SimulatorError.transport("storage receipt unavailable")) }
        else { pending?.resume(returning: receipt) }
    }
}

extension SimulatorESP32LifecycleTests {
    @MainActor func testConcurrentStopsWaitForOneStorageFinish() async {
        let gate = StorageFinishGate()
        let model = StandaloneRuntimeModel(
            firmwareService: BundledFirmwareService(run: { try await gate.run($0) }),
            instanceID: UUID(), ownedStorageSession: URL(fileURLWithPath: "/private/tmp/authored-storage-session"))
        let first = Task { await model.stop() }
        await gate.waitUntilEntered()
        var secondReturned = false
        let second = Task { let result = await model.stop(); secondReturned = true; return result }
        try? await Task.sleep(for: .milliseconds(50))
        XCTAssertFalse(secondReturned)
        let count = await gate.count()
        XCTAssertEqual(count, 1)
        await gate.release()
        let firstResult = await first.value
        let secondResult = await second.value
        XCTAssertTrue(firstResult)
        XCTAssertTrue(secondResult)
        XCTAssertTrue(model.instanceStorageMessage.contains("已保存"))
    }

    @MainActor func testStorageFinishFailureRemainsRetryable() async {
        let gate = StorageFinishGate()
        let model = StandaloneRuntimeModel(
            firmwareService: BundledFirmwareService(run: { try await gate.run($0) }),
            instanceID: UUID(), ownedStorageSession: URL(fileURLWithPath: "/private/tmp/authored-storage-session"))
        let first = Task { await model.stop() }
        await gate.waitUntilEntered()
        await gate.release(fail: true)
        let failed = await first.value
        XCTAssertFalse(failed)
        XCTAssertTrue(model.instanceStorageMessage.contains("提交未完成"))
        let retried = await model.stop()
        XCTAssertTrue(retried)
        let count = await gate.count()
        XCTAssertEqual(count, 2)
        XCTAssertTrue(model.instanceStorageMessage.contains("已保存"))
    }
}
