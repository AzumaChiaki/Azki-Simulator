import Foundation
import IOKit
import IOKit.serial

nonisolated enum SimulatorBluetoothMode: String, Codable, CaseIterable, Sendable {
    case virtual
    case esp32
    var label: String { self == .virtual ? "Mac 蓝牙广播" : "ESP32 串口桥接" }
}

nonisolated struct SimulatorSerialDevice: Identifiable, Equatable, Sendable {
    let path: String
    let name: String
    var id: String { path }
    var label: String { "\(name) — \(path)" }

    static func validatePath(_ path: String) throws {
        guard path.hasPrefix("/dev/cu."), path.utf8.count <= 255,
              !path.dropFirst(5).contains("/"), !path.contains("\0"),
              !path.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else {
            throw SimulatorError.transport("请选择已连接的 ESP32 串口设备")
        }
    }

    private static func hasUSBAncestor(_ device: io_registry_entry_t) -> Bool {
        var entry = device
        IOObjectRetain(entry)
        while entry != 0 {
            let vendor = IORegistryEntryCreateCFProperty(entry, "idVendor" as CFString,
                kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
            let product = IORegistryEntryCreateCFProperty(entry, "idProduct" as CFString,
                kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
            if vendor != nil && product != nil {
                IOObjectRelease(entry)
                return true
            }
            var parent: io_registry_entry_t = 0
            let status = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent)
            IOObjectRelease(entry)
            guard status == KERN_SUCCESS else { return false }
            entry = parent
        }
        return false
    }

    static func connected() -> [Self] {
        guard let matching = IOServiceMatching(kIOSerialBSDServiceValue) else { return [] }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var result: [Self] = []
        while case let device = IOIteratorNext(iterator), device != 0 {
            defer { IOObjectRelease(device) }
            guard hasUSBAncestor(device),
                  let path = IORegistryEntryCreateCFProperty(device, kIOCalloutDeviceKey as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String,
                  (try? validatePath(path)) != nil else { continue }
            let name = IORegistryEntryCreateCFProperty(device, kIOTTYDeviceKey as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String
            result.append(Self(path: path, name: name ?? URL(fileURLWithPath: path).lastPathComponent))
        }
        return result.sorted { $0.path < $1.path }
    }
}
