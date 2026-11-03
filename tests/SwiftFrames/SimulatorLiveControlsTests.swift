import XCTest
import Foundation
@testable import AzkiSimulator

/// Pure value/contract tests: no session, socket, subprocess or firmware.
final class SimulatorLiveControlsTests: XCTestCase {
    private func report() -> SimulatorJSON {
        .object([
            "supported": .object(["steps": .bool(true), "heartRate": .bool(true), "charger": .bool(true),
                                  "batteryLevel": .bool(true), "batteryRaw": .bool(true), "temperature": .bool(false), "clock": .bool(false)]),
            "notes": .object(["clock": .string("boot only")]),
            "state": .object(["walkSteps": .number(100), "walked": .number(20), "cadence": .number(110),
                              "heartRate": .number(72), "worn": .bool(true), "charger": .string("unplugged"),
                              "batteryRaw": .number(12_000), "temperatureC": .null, "clock": .null])])
    }

    private func replace(_ json: SimulatorJSON, section: String, key: String, value: SimulatorJSON?) -> SimulatorJSON {
        guard case .object(var object) = json, case .object(var child)? = object[section] else { preconditionFailure() }
        child[key] = value
        object[section] = .object(child)
        return .object(object)
    }

    func testCompleteLegacySnapshotAndNewAcknowledgement() throws {
        let old = report()
        let legacy = try SimulatorLiveWriteReceipt(old)
        XCTAssertEqual(legacy.snapshot?.stepsRemaining, 80)
        XCTAssertFalse(legacy.guestObserved)
        let receipt = try SimulatorLiveWriteReceipt(.object([
            "accepted": .bool(true), "guestObserved": .bool(false), "state": old]))
        XCTAssertEqual(receipt, legacy)
        let unavailable = try SimulatorLiveWriteReceipt(.object([
            "accepted": .bool(true), "guestObserved": .bool(false), "stateUnavailable": .bool(true)]))
        XCTAssertNil(unavailable.snapshot)
        XCTAssertFalse(unavailable.guestObserved)
    }

    func testMalformedSuccessfulReplyCannotConfirmWrite() throws {
        let emptyBridge = try JSONDecoder().decode(SimulatorBridgeResponse.self, from: Data(#"{"ok":true}"#.utf8))
        let invalid: [SimulatorJSON] = [try emptyBridge.checkedResult(), .object([:]),
            .object(["supported": .object([:]), "notes": .object([:]), "state": .object([:])]),
            .object(["accepted": .bool(true), "guestObserved": .bool(false), "state": .object([:])]),
            .object(["accepted": .bool(true), "guestObserved": .bool(true), "state": report()]),
            .object(["accepted": .bool(true), "guestObserved": .bool(false), "stateUnavailable": .bool(false)]),
            .object(["accepted": .bool(false), "guestObserved": .bool(false), "state": report()])]
        for value in invalid {
            XCTAssertThrowsError(try SimulatorLiveWriteReceipt(value)) { XCTAssertTrue($0 is SimulatorLiveOutcomeUnknown) }
        }
    }

    func testMalformedStateAndCapabilityTypesAreRejected() {
        let cases = [
            replace(report(), section: "supported", key: "steps", value: .number(1)),
            replace(report(), section: "supported", key: "heartRate", value: nil),
            replace(report(), section: "state", key: "worn", value: .number(1)),
            replace(report(), section: "state", key: "walkSteps", value: .number(-1)),
            replace(report(), section: "state", key: "cadence", value: .number(40.5)),
            replace(report(), section: "state", key: "batteryRaw", value: .number(65_536)),
            replace(report(), section: "state", key: "charger", value: .string("unknown")),
            replace(report(), section: "state", key: "temperatureC", value: .number(20)),
            replace(report(), section: "notes", key: "clock", value: .bool(false))]
        for value in cases { XCTAssertNil(SimulatorLiveState(value)) }
    }

    func testOptionalOrientationAndSixAxisDoNotBreakSnapshot() {
        var value = replace(report(), section: "supported", key: "orientation", value: .bool(true))
        value = replace(value, section: "state", key: "orientation", value: .null)
        value = replace(value, section: "supported", key: "motionRaw", value: .bool(true))
        value = replace(value, section: "state", key: "motionRaw", value: .object(["mode": .string("auto")]))
        XCTAssertNotNil(SimulatorLiveState(value))
        let motion: SimulatorJSON = .object(["mode": .string("raw"), "accel": .array([.number(-32_768), .number(0), .number(32_767)]),
                                            "gyro": .array([.number(1), .number(2), .number(3)])])
        value = replace(value, section: "state", key: "motionRaw", value: motion)
        XCTAssertNotNil(SimulatorLiveState(value))
        XCTAssertNil(SimulatorLiveState(replace(value, section: "supported", key: "motionRaw", value: .bool(false))))
        XCTAssertNil(SimulatorLiveState(replace(value, section: "state", key: "motionRaw", value: .object([
            "mode": .string("raw"), "accel": .array([.number(0)]), "gyro": .array([.number(1), .number(2), .number(3)])]))))
    }

    func testSensorBusObservationIsNotPromotedToGuestObservation() throws {
        guard case .object(var value) = report() else { return XCTFail("invalid fixture") }
        value["sensorObservation"] = .object(["scope": .string("sensor-bus"), "guestObserved": .bool(false)])
        // The counters are deliberately opaque here; Runtime owns their ABI.
        let receipt = try SimulatorLiveWriteReceipt(.object([
            "accepted": .bool(true), "guestObserved": .bool(false), "state": .object(value)]))
        XCTAssertNotNil(receipt.snapshot)
        XCTAssertFalse(receipt.guestObserved)
        value["sensorObservation"] = .string("invalid")
        XCTAssertNil(SimulatorLiveState(.object(value)))
    }

    func testUnsupportedHealthAndBatteryStayUnavailable() {
        let value: SimulatorJSON = .object([
            "supported": .object(["steps": .bool(false), "heartRate": .bool(false), "charger": .bool(false),
                                  "batteryLevel": .bool(false), "batteryRaw": .bool(false), "temperature": .bool(false), "clock": .bool(false)]),
            "notes": .object([:]),
            "state": .object(["walkSteps": .null, "walked": .null, "cadence": .null, "heartRate": .null,
                              "worn": .null, "charger": .null, "batteryRaw": .null, "temperatureC": .null, "clock": .null])])
        let snapshot = SimulatorLiveState(value)
        XCTAssertNotNil(snapshot)
        for key in ["steps", "heartRate", "batteryLevel", "charger"] { XCTAssertEqual(snapshot?.supports(key), false) }
    }

    func testLiveSuccessThenSaveFailureRetainsPartialOutcome() {
        var progress = SimulatorLiveApplyProgress()
        progress.hardwareAccepted = true
        let message = progress.message(failure: "disk failure") ?? ""
        XCTAssertTrue(message.contains("模拟硬件已接受输入"))
        XCTAssertTrue(message.contains("下次开机的值未确认保存"))
        XCTAssertFalse(message.contains("已保存为下次开机"))
        XCTAssertTrue(message.contains("disk failure"))
        progress.nextBootSaved = true
        XCTAssertTrue(progress.message()?.contains("已保存为下次开机") == true)
        progress.stateUnavailable = true
        XCTAssertTrue(progress.message()?.contains("不要重复提交") == true)
        XCTAssertFalse(progress.message()?.contains("已立即生效") == true)
    }

    func testDeliveryClassificationPreservesKnownFailuresAndUnknownOutcome() {
        let failure = SimulatorError.transport("connection interrupted")
        XCTAssertTrue(SimulatorLiveDelivery.error(failure, command: "live-set", submitted: true, receivedReply: false) is SimulatorLiveOutcomeUnknown)
        XCTAssertEqual(SimulatorLiveDelivery.error(failure, command: "live-set", submitted: false, receivedReply: false) as? SimulatorError, failure)
        XCTAssertEqual(SimulatorLiveDelivery.error(failure, command: "live-get", submitted: true, receivedReply: false) as? SimulatorError, failure)
        let rejection = SimulatorError.remote("unsupported [unsupported_hardware]")
        XCTAssertEqual(SimulatorLiveDelivery.error(rejection, command: "live-set", submitted: true, receivedReply: true) as? SimulatorError, rejection)
    }

    func testPollFailureMarksOldSnapshotAsStale() {
        XCTAssertTrue(SimulatorLiveFreshness.failureMessage("timeout", hasSnapshot: true).contains("可能已过期"))
        XCTAssertFalse(SimulatorLiveFreshness.failureMessage("timeout", hasSnapshot: false).contains("上次读取"))
        XCTAssertTrue(SimulatorLiveFreshness.failureMessage("[unsupported_hardware]", hasSnapshot: false).contains("下次开机"))
    }
}
