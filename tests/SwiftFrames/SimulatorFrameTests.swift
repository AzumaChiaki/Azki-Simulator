import XCTest
import Foundation
import Darwin
@testable import AzkiSimulator

final class SimulatorFrameTests: XCTestCase {
    private func frame(format: UInt32 = 2, sequence: UInt32 = 2, width: Int = 432, height: Int = 514,
                       stride: Int? = nil) -> Data {
        let bpp = format == 1 ? 2 : 3
        var bytes = Data("Q66FRAME".utf8)
        for word in [1, sequence, UInt32(width), UInt32(height), UInt32(stride ?? width * bpp), format] {
            var little = word.littleEndian
            withUnsafeBytes(of: &little) { bytes.append(contentsOf: $0) }
        }
        let pixel: [UInt8] = format == 1 ? [0x71, 0x8e] : [7, 11, 19]
        for _ in 0..<(width * height) { bytes.append(contentsOf: pixel) }
        return bytes
    }

    func testNativeRGB888PreservesLowBitsAndLegacy565Matches() throws {
        for geometry in SimulatorDisplayGeometry.allCases {
            let rgb = try SimulatorFrame.decode(frame(width: geometry.width, height: geometry.height), expectedSize: geometry.size)
            XCTAssertEqual(Data([7, 11, 19, 255]), rgb.rgba.prefix(4))
            XCTAssertEqual(geometry.width * geometry.height * 4, rgb.rgba.count)
            let old = try SimulatorFrame.decode(frame(format: 1, width: geometry.width, height: geometry.height), expectedSize: geometry.size)
            XCTAssertEqual(Data([136, 204, 136, 255]), old.rgba.prefix(4))
        }
    }

    func testWrongStrideUnsupportedFormatAndTruncationRejected() {
        for format: UInt32 in [1, 2] {
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format, stride: 432)))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format, stride: 432 * 4)))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format).dropLast()))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format) + Data([0])))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format, sequence: 1)))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format, sequence: 0)))
            XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: format), expectedSize: CGSize(width: 212, height: 520)))
        }
        XCTAssertThrowsError(try SimulatorFrame.decode(frame(format: 3)))
    }

    func testIndependentActorReadsBothFormatsAndRejectsInvalidUnchangedFrames() async throws {
        for format: UInt32 in [1, 2] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("azki-frame-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            defer { try? FileManager.default.removeItem(at: root) }
            let manifest = SimulatorSessionManifest(version: 1, id: UUID(), socket: "control.sock", framebuffer: "frame.raw",
                width: 432, height: 514, capabilities: [])
            try JSONEncoder().encode(manifest).write(to: root.appendingPathComponent("session.json"))
            let path = root.appendingPathComponent("frame.raw")
            try frame(format: format).write(to: path)
            let connection = try SimulatorSessionConnection(directoryURL: root)
            let reader = try await connection.makeFrameReader()
            let first = try await reader.presentation(after: nil)
            XCTAssertEqual(first?.frame.rgba.prefix(4), Data(format == 1 ? [136, 204, 136, 255] : [7, 11, 19, 255]))
            XCTAssertNotNil(first?.image)
            let unchanged = try await reader.presentation(after: 2)
            XCTAssertNil(unchanged)
            for bad in [frame(format: format, stride: 432), frame(format: format).dropLast(), frame(format: format, sequence: 3)] {
                try Data(bad).write(to: path)
                do {
                    _ = try await reader.presentation(after: 2)
                    XCTFail("Invalid unchanged frame was accepted")
                } catch { }
            }
            try frame(format: format, sequence: 4).write(to: path)
            let next = try await reader.presentation(after: 2)
            XCTAssertEqual(next?.frame.sequence, 4)
            try FileManager.default.removeItem(at: path)
            let target = root.appendingPathComponent("other.raw")
            try frame(format: format).write(to: target)
            try FileManager.default.createSymbolicLink(at: path, withDestinationURL: target)
            do {
                _ = try await reader.presentation(after: nil)
                XCTFail("Symlink frame was accepted")
            } catch { }
            try FileManager.default.removeItem(at: path)
            XCTAssertEqual(link(target.path, path.path), 0)
            do {
                _ = try await reader.presentation(after: nil)
                XCTFail("Hardlinked frame was accepted")
            } catch { }
            await reader.close()
        }
    }
}
