import Foundation
import CoreGraphics

/// Finite display contract for the imported Xiaomi Band profiles. A generic
/// codec fixture may be smaller; live sessions and input accept only these pairs.
nonisolated enum SimulatorDisplayGeometry: CaseIterable, Equatable, Sendable {
    case pixels192x490, pixels212x520, pixels336x480

    var width: Int {
        switch self { case .pixels192x490: 192; case .pixels212x520: 212; case .pixels336x480: 336 }
    }
    var height: Int {
        switch self { case .pixels192x490: 490; case .pixels212x520: 520; case .pixels336x480: 480 }
    }
    var size: CGSize { CGSize(width: width, height: height) }
    init?(width: Int, height: Int) {
        guard let match = Self.allCases.first(where: { $0.width == width && $0.height == height }) else { return nil }
        self = match
    }
    init?(size: CGSize) {
        guard let match = Self.allCases.first(where: { $0.size == size }) else { return nil }
        self = match
    }
}

nonisolated enum SimulatorError: LocalizedError, Equatable, Sendable {
    case invalidSession(String)
    case unsafeFile(String)
    case invalidFrame(String)
    case frameBeingWritten
    case disconnected
    case transport(String)
    case remote(String)

    var errorDescription: String? {
        switch self {
        case .invalidSession(let detail): "会话无效：\(detail)"
        case .unsafeFile(let detail): "已拒绝不安全文件：\(detail)"
        case .invalidFrame(let detail): "画面无效：\(detail)"
        case .frameBeingWritten: "画面正在更新"
        case .disconnected: "模拟器会话已断开"
        case .transport(let detail): "本机连接失败：\(detail)"
        case .remote(let detail): detail
        }
    }
}

/// Wire format: <8s6I, followed by height rows of little-endian RGB565.
/// An odd sequence means the producer is writing. The reader also compares the
/// header before/after copying; an even value alone is not a complete seqlock.
nonisolated struct SimulatorFrameHeader: Equatable, Sendable {
    static let byteCount = 32
    static let maximumBytes = 8 * 1_024 * 1_024
    let sequence: UInt32
    let width: Int
    let height: Int
    let stride: Int

    var byteCount: Int { Self.byteCount + stride * height }

    static func decode(_ data: Data) throws -> Self {
        guard data.count >= byteCount, Array(data.prefix(8)) == Array("Q66FRAME".utf8) else {
            throw SimulatorError.invalidFrame("文件头不匹配")
        }
        func word(_ offset: Int) -> UInt32 {
            (0..<4).reduce(UInt32(0)) { $0 | UInt32(data[data.startIndex + offset + $1]) << ($1 * 8) }
        }
        guard word(8) == 1, word(28) == 1 else { throw SimulatorError.invalidFrame("版本或像素格式不受支持") }
        let sequence = word(12)
        guard sequence != 0, sequence & 1 == 0 else { throw SimulatorError.frameBeingWritten }
        let width = Int(word(16)), height = Int(word(20)), stride = Int(word(24))
        guard (1...2_048).contains(width), (1...2_048).contains(height),
              stride >= width * 2, stride <= 8_192, stride.isMultiple(of: 2),
              stride * height <= maximumBytes - byteCount else {
            throw SimulatorError.invalidFrame("尺寸、行宽或长度超限")
        }
        return Self(sequence: sequence, width: width, height: height, stride: stride)
    }
}

nonisolated struct SimulatorFrame: Sendable {
    let sequence: UInt32
    let width: Int
    let height: Int
    let rgba: Data

    static func decode(_ data: Data, expectedSize: CGSize? = nil) throws -> Self {
        let header = try SimulatorFrameHeader.decode(data)
        guard data.count == header.byteCount else { throw SimulatorError.invalidFrame("像素长度不匹配") }
        if let expectedSize {
            guard let geometry = SimulatorDisplayGeometry(size: expectedSize),
                  header.width == geometry.width, header.height == geometry.height,
                  header.stride == geometry.width * 2 else {
                throw SimulatorError.invalidFrame("画面尺寸或行宽与已验证会话不一致")
            }
        }
        var output = Data(count: header.width * header.height * 4)
        data.withUnsafeBytes { sourceBytes in
            output.withUnsafeMutableBytes { outputBytes in
                let source = sourceBytes.bindMemory(to: UInt8.self)
                let destination = outputBytes.bindMemory(to: UInt8.self)
                for y in 0..<header.height {
                    for x in 0..<header.width {
                        let index = SimulatorFrameHeader.byteCount + y * header.stride + x * 2
                        let pixel = UInt16(source[index]) | UInt16(source[index + 1]) << 8
                        let target = (y * header.width + x) * 4
                        let red = UInt8((pixel >> 11) & 31), green = UInt8((pixel >> 5) & 63), blue = UInt8(pixel & 31)
                        // Match the Q66 core's existing screenshot definition:
                        // shift only, without introducing replicated low bits.
                        destination[target] = red << 3
                        destination[target + 1] = green << 2
                        destination[target + 2] = blue << 3
                        destination[target + 3] = 255
                    }
                }
            }
        }
        return Self(sequence: header.sequence, width: header.width, height: header.height, rgba: output)
    }

    func makeCGImage() -> CGImage? {
        guard let provider = CGDataProvider(data: rgba as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: width * 4, space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue).union(.byteOrder32Big),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

nonisolated struct SimulatorInputEvent: Codable, Equatable, Sendable {
    let x: Int
    let y: Int
    let down: Bool
}

/// Both values are immutable; the CGImage retains its own immutable Data
/// provider. Construct it on the frame executor, then publish on the main actor.
nonisolated struct SimulatorFramePresentation: @unchecked Sendable {
    let frame: SimulatorFrame
    let image: CGImage?
}

/// Fixed deadlines avoid adding decode/IO time to every display interval.
/// At most one read is in flight. Missed deadlines are skipped, never queued.
nonisolated struct SimulatorFrameCadence {
    static let interval: Duration = .nanoseconds(16_666_667)
    private var deadline: ContinuousClock.Instant

    init(now: ContinuousClock.Instant) { deadline = now }

    mutating func next(after now: ContinuousClock.Instant) -> ContinuousClock.Instant {
        deadline += Self.interval
        if deadline <= now { deadline = now + Self.interval }
        return deadline
    }
}

/// The device canvas may zoom; application controls always retain shared sizes.
nonisolated enum SimulatorCanvasGeometry {
    static func contentRect(in bounds: CGRect, pixelSize: CGSize) -> CGRect {
        guard bounds.origin.x.isFinite, bounds.origin.y.isFinite,
              bounds.width.isFinite, bounds.height.isFinite, pixelSize.width.isFinite, pixelSize.height.isFinite,
              bounds.width > 0, bounds.height > 0, pixelSize.width > 0, pixelSize.height > 0 else { return .zero }
        let scale = min(bounds.width / pixelSize.width, bounds.height / pixelSize.height)
        let size = CGSize(width: pixelSize.width * scale, height: pixelSize.height * scale)
        return CGRect(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2, width: size.width, height: size.height)
    }

    static func input(at point: CGPoint, bounds: CGRect, pixelSize: CGSize, down: Bool) -> SimulatorInputEvent? {
        guard SimulatorDisplayGeometry(size: pixelSize) != nil else { return nil }
        let rect = contentRect(in: bounds, pixelSize: pixelSize)
        guard point.x.isFinite, point.y.isFinite, rect.width > 0, rect.height > 0,
              point.x >= rect.minX, point.x < rect.maxX, point.y >= rect.minY, point.y < rect.maxY else { return nil }
        return SimulatorInputEvent(x: min(Int(pixelSize.width) - 1, Int((point.x - rect.minX) / rect.width * pixelSize.width)),
                                   y: min(Int(pixelSize.height) - 1, Int((point.y - rect.minY) / rect.height * pixelSize.height)), down: down)
    }
}
