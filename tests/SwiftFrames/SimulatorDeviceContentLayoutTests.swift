import XCTest
import Foundation
@testable import AzkiSimulator

final class SimulatorDeviceContentLayoutTests: XCTestCase {
    private typealias Layout = SimulatorDeviceContentLayout

    func testDefaultHeightKeepsCanvasAboveMinimum() {
        let available: CGFloat = 700
        let log = Layout.logPaneHeight(availableHeight: available, preferred: Layout.defaultLogPaneHeight)
        let canvas = Layout.canvasHeight(availableHeight: available, logPaneHeight: log)
        XCTAssertEqual(log, 320)
        XCTAssertEqual(canvas, available - Layout.dividerHeight - log)
        XCTAssertGreaterThanOrEqual(canvas, Layout.minimumCanvasHeight)
        XCTAssertEqual(log + Layout.dividerHeight + canvas, available)
    }

    func testResizingShrinksLogPaneBeforeCanvas() {
        // Shrinking the window must reduce the log pane, not push the canvas
        // out of the measured container (the old VSplitView failure mode).
        let available: CGFloat = 460
        let log = Layout.logPaneHeight(availableHeight: available, preferred: Layout.defaultLogPaneHeight)
        let canvas = Layout.canvasHeight(availableHeight: available, logPaneHeight: log)
        XCTAssertEqual(log, available - Layout.dividerHeight - Layout.minimumCanvasHeight)
        XCTAssertEqual(canvas, Layout.minimumCanvasHeight)
        XCTAssertEqual(log + Layout.dividerHeight + canvas, available)
    }

    func testDraggedPreferenceIsClampedToAvailableSpace() {
        let available: CGFloat = 760
        let log = Layout.logPaneHeight(availableHeight: available, preferred: 1_000)
        let canvas = Layout.canvasHeight(availableHeight: available, logPaneHeight: log)
        XCTAssertEqual(log, available - Layout.dividerHeight - Layout.minimumCanvasHeight)
        XCTAssertEqual(canvas, Layout.minimumCanvasHeight)
        XCTAssertEqual(log + Layout.dividerHeight + canvas, available)
    }

    func testSmallOrInvalidContainersNeverProduceNegativeOrNaN() {
        for available: CGFloat in [0, 100, 200, 360, 368, .infinity, .nan] {
            let log = Layout.logPaneHeight(availableHeight: available, preferred: 320)
            let canvas = Layout.canvasHeight(availableHeight: available, logPaneHeight: log)
            XCTAssertTrue(log.isFinite, "log height for \(available)")
            XCTAssertGreaterThanOrEqual(log, 0, "log height for \(available)")
            XCTAssertTrue(canvas.isFinite, "canvas height for \(available)")
            XCTAssertGreaterThanOrEqual(canvas, 0, "canvas height for \(available)")
        }
    }

    func testInvalidPreferredFallsBackToDefault() {
        let log = Layout.logPaneHeight(availableHeight: 700, preferred: .nan)
        XCTAssertEqual(log, Layout.defaultLogPaneHeight)
    }
}
