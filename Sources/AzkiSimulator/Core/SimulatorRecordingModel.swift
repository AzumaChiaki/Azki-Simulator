import AVFoundation
import CoreGraphics
import CoreVideo
import Foundation
import Observation

/// Records this instance's framebuffer, never the macOS screen or another app.
/// There is one sampled frame in flight; the encoder drops samples under pressure.
@MainActor @Observable
final class SimulatorRecordingModel {
    private(set) var isRecording = false
    private(set) var isFinishing = false
    private(set) var elapsedTime: TimeInterval = 0
    private(set) var savedFileURL: URL?
    private(set) var recoveryFileURL: URL?
    var errorMessage: String?

    @ObservationIgnored private let outputDirectory: URL
    @ObservationIgnored private let publishRecording: @Sendable (URL, URL) throws -> Void
    @ObservationIgnored private var encoder: SimulatorRecordingEncoder?
    @ObservationIgnored private var sampling: Task<Void, Never>?
    @ObservationIgnored private var finishing: Task<Void, Never>?
    @ObservationIgnored private var source: (() -> CGImage?)?
    @ObservationIgnored private var started: ContinuousClock.Instant?
    @ObservationIgnored private var generation = UUID()
    @ObservationIgnored private let clock = ContinuousClock()

    init(outputDirectory: URL? = nil,
         publishRecording: @escaping @Sendable (URL, URL) throws -> Void = { source, destination in
             try FileManager.default.moveItem(at: source, to: destination)
         }) {
        self.outputDirectory = outputDirectory ?? FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
        self.publishRecording = publishRecording
    }

    func start(session: SimulatorSessionModel, instanceName: String) {
        guard session.isRunning, session.image != nil, let sessionID = session.manifest?.id else {
            errorMessage = "请等待设备启动并显示画面后录制"
            return
        }
        start(instanceName: instanceName) { [weak session] in
            guard let session, session.isRunning, session.manifest?.id == sessionID else { return nil }
            return session.image
        }
    }

    /// An injectable framebuffer source also permits tests without starting a guest.
    /// nil means the originating session ended; a new session is never spliced in.
    func start(instanceName: String, frameSource: @escaping () -> CGImage?) {
        guard !isRecording, !isFinishing, finishing == nil else { return }
        errorMessage = nil
        savedFileURL = nil
        recoveryFileURL = nil
        elapsedTime = 0
        guard let firstFrame = frameSource() else {
            errorMessage = "请等待设备启动并显示画面后录制"
            return
        }
        do {
            encoder = try SimulatorRecordingEncoder(image: firstFrame, instanceName: instanceName,
                                                    outputDirectory: outputDirectory, publishRecording: publishRecording)
            source = frameSource
            started = clock.now
            generation = UUID()
            let token = generation
            isRecording = true
            sampling = Task { [weak self] in
                // Sampling unchanged images is intentional: a static watchface
                // must retain its real duration in the resulting movie.
                while !Task.isCancelled {
                    guard await self?.sample(token: token) == true else { return }
                    do { try await Task.sleep(for: .nanoseconds(33_333_333)) }
                    catch { return }
                }
            }
        } catch {
            errorMessage = "无法开始录屏：\(error.localizedDescription)"
        }
    }

    /// Call before instance teardown and await before application termination.
    /// Concurrent stop requests share one finalization operation.
    func stop() async {
        if let finishing { await finishing.value; return }
        guard let encoder else { return }
        sampling?.cancel()
        sampling = nil
        source = nil
        isRecording = false
        isFinishing = true
        let end = elapsedSeconds()
        elapsedTime = end
        let operation = Task { [weak self] in
            do {
                let saved = try await encoder.finish(at: end)
                self?.savedFileURL = saved
            } catch {
                self?.recoveryFileURL = await encoder.recoverableFileURL()
                let recovery = self?.recoveryFileURL.map { "；临时文件保留在 \($0.path)" } ?? ""
                self?.errorMessage = "录屏保存失败：\(error.localizedDescription)\(recovery)"
            }
            self?.encoder = nil
            self?.started = nil
        }
        finishing = operation
        await operation.value
        finishing = nil
        isFinishing = false
    }

    private func sample(token: UUID) async -> Bool {
        guard isRecording, generation == token, let encoder else { return false }
        guard let image = source?() else { await stop(); return false }
        elapsedTime = elapsedSeconds()
        do { try await encoder.append(image, at: elapsedTime) }
        catch {
            guard generation == token else { return false }
            errorMessage = "录屏已结束：\(error.localizedDescription)"
            await stop()
            return false
        }
        return isRecording && generation == token
    }

    private func elapsedSeconds() -> TimeInterval {
        guard let started else { return elapsedTime }
        let components = started.duration(to: clock.now).components
        return Double(components.seconds) + Double(components.attoseconds) / 1e18
    }
}

private enum SimulatorRecordingError: LocalizedError {
    case invalidGeometry, changedGeometry, unavailableBuffer, cannotDraw, writer(String), noFrames
    var errorDescription: String? {
        switch self {
        case .invalidGeometry: "画面尺寸不适合 H.264 录制"
        case .changedGeometry: "设备画面尺寸已改变，原尺寸录像已结束"
        case .unavailableBuffer: "无法分配录屏缓冲区"
        case .cannotDraw: "无法转换录屏画面"
        case .writer(let detail): detail
        case .noFrames: "编码器尚未接收画面"
        }
    }
}

/// All mutable AVFoundation objects remain on one executor. Pool allocation is
/// limited to six buffers, independent of recording duration or guest frame rate.
private actor SimulatorRecordingEncoder {
    private let writer: AVAssetWriter
    private let input: AVAssetWriterInput
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor
    private let width: Int
    private let height: Int
    private let stagingDirectory: URL
    private let stagingURL: URL
    private let destinationURL: URL
    private let publishRecording: @Sendable (URL, URL) throws -> Void
    private var frameCount = 0
    private var lastTime = CMTime.zero
    private var completedURL: URL?

    init(image: CGImage, instanceName: String, outputDirectory: URL,
         publishRecording: @escaping @Sendable (URL, URL) throws -> Void) throws {
        self.publishRecording = publishRecording
        width = image.width
        height = image.height
        guard width > 0, height > 0, width <= 2_048, height <= 2_048,
              width.isMultiple(of: 2), height.isMultiple(of: 2) else {
            throw SimulatorRecordingError.invalidGeometry
        }
        let fileManager = FileManager.default
        // Stage beside the final output so the final rename remains on one
        // filesystem. A failed encode/move keeps this private recovery directory.
        let unique = UUID().uuidString.lowercased()
        stagingDirectory = outputDirectory.appendingPathComponent(".azki-recording-\(unique)", isDirectory: true)
        stagingURL = stagingDirectory.appendingPathComponent("recording.mp4")
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd-HH-mm-ss"
        let name = Self.safeName(instanceName)
        destinationURL = outputDirectory.appendingPathComponent("Azki-\(name)-\(formatter.string(from: Date()))-\(unique).mp4")
        try fileManager.createDirectory(at: stagingDirectory, withIntermediateDirectories: false,
                                        attributes: [.posixPermissions: 0o700])
        do {
            writer = try AVAssetWriter(outputURL: stagingURL, fileType: .mp4)
            input = AVAssetWriterInput(mediaType: .video, outputSettings: [
                AVVideoCodecKey: AVVideoCodecType.h264,
                AVVideoWidthKey: width,
                AVVideoHeightKey: height,
                AVVideoColorPropertiesKey: [
                    AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                    AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                    AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2
                ],
                AVVideoCompressionPropertiesKey: [
                    AVVideoAverageBitRateKey: min(8_000_000, max(400_000, width * height * 10)),
                    AVVideoExpectedSourceFrameRateKey: 30,
                    AVVideoMaxKeyFrameIntervalKey: 60,
                    AVVideoAllowFrameReorderingKey: false
                ]
            ])
            input.expectsMediaDataInRealTime = true
            adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input,
                sourcePixelBufferAttributes: [
                    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                    kCVPixelBufferWidthKey as String: width,
                    kCVPixelBufferHeightKey as String: height,
                    kCVPixelBufferCGImageCompatibilityKey as String: true,
                    kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
                    kCVPixelBufferIOSurfacePropertiesKey as String: [:]
                ])
            guard writer.canAdd(input) else { throw SimulatorRecordingError.writer("系统不支持此录屏编码配置") }
            writer.add(input)
            guard writer.startWriting() else {
                throw SimulatorRecordingError.writer(writer.error?.localizedDescription ?? "编码器启动失败")
            }
            writer.startSession(atSourceTime: .zero)
        } catch {
            // Initialization has not accepted any frame; only an empty staging
            // directory/file can exist here, so there is no recording to retain.
            try? fileManager.removeItem(at: stagingDirectory)
            throw error
        }
    }

    func append(_ image: CGImage, at seconds: TimeInterval) throws {
        guard image.width == width, image.height == height else { throw SimulatorRecordingError.changedGeometry }
        guard writer.status == .writing else {
            throw SimulatorRecordingError.writer(writer.error?.localizedDescription ?? "编码器已停止")
        }
        guard input.isReadyForMoreMediaData else { return }
        guard let pool = adaptor.pixelBufferPool else { throw SimulatorRecordingError.unavailableBuffer }
        var buffer: CVPixelBuffer?
        let result = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool,
            [kCVPixelBufferPoolAllocationThresholdKey: 6] as CFDictionary, &buffer)
        if result == kCVReturnWouldExceedAllocationThreshold { return }
        guard result == kCVReturnSuccess, let buffer else { throw SimulatorRecordingError.unavailableBuffer }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { throw SimulatorRecordingError.cannotDraw }
        context.interpolationQuality = .none
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        // First sample starts at zero. Later samples use monotonic elapsed time,
        // so dropped frames and temporary backpressure do not shorten the movie.
        let time = frameCount == 0 ? CMTime.zero : CMTime(seconds: seconds, preferredTimescale: 60_000)
        guard frameCount == 0 || time > lastTime else { return }
        guard adaptor.append(buffer, withPresentationTime: time) else {
            throw SimulatorRecordingError.writer(writer.error?.localizedDescription ?? "写入录屏帧失败")
        }
        frameCount += 1
        lastTime = time
    }

    func finish(at seconds: TimeInterval) async throws -> URL {
        if let completedURL { return completedURL }
        guard frameCount > 0 else { writer.cancelWriting(); throw SimulatorRecordingError.noFrames }
        guard writer.status == .writing else {
            throw SimulatorRecordingError.writer(writer.error?.localizedDescription ?? "编码器异常结束")
        }
        let end = max(CMTime(seconds: seconds, preferredTimescale: 60_000), lastTime + CMTime(value: 1, timescale: 30))
        writer.endSession(atSourceTime: end)
        input.markAsFinished()
        await writer.finishWriting()
        guard writer.status == .completed else {
            throw SimulatorRecordingError.writer(writer.error?.localizedDescription ?? "视频文件未能完成")
        }
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: stagingURL.path)
        // moveItem refuses an existing destination. Never remove/replace a file.
        try publishRecording(stagingURL, destinationURL)
        completedURL = destinationURL
        try? FileManager.default.removeItem(at: stagingDirectory)
        return destinationURL
    }

    func recoverableFileURL() -> URL? {
        FileManager.default.fileExists(atPath: stagingURL.path) ? stagingURL : nil
    }

    private static func safeName(_ proposed: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: " -_"))
        var result = ""
        for scalar in proposed.unicodeScalars {
            let next = allowed.contains(scalar) ? String(scalar) : "-"
            guard result.utf8.count + next.utf8.count <= 96 else { break }
            result += next
        }
        let trimmed = result.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "设备" : trimmed
    }
}
