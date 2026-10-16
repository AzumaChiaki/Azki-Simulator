import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Saves the device frame shown on screen as a PNG on the Desktop, next to
/// the screen recordings and named the same way.
nonisolated enum SimulatorScreenshot {
    static func save(_ image: CGImage, instanceName: String,
                     directory: URL = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]) throws -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd-HH-mm-ss"
        let stamp = formatter.string(from: Date())
        let name = safeName(instanceName)
        var url = directory.appendingPathComponent("Azki-\(name)-\(stamp).png")
        var index = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = directory.appendingPathComponent("Azki-\(name)-\(stamp)-\(index).png")
            index += 1
        }
        guard let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
            throw SimulatorError.invalidSession("无法在桌面创建截图文件")
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw SimulatorError.invalidSession("截图保存失败")
        }
        return url
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
        return trimmed.isEmpty ? "Simulator" : trimmed
    }
}
