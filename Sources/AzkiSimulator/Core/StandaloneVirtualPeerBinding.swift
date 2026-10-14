import CoreFoundation
import Darwin
import Foundation

/// Grants one immutable synthetic state input. Pending changes live only in
/// this owned run and are accepted later by the joint storage transaction.
struct StandaloneVirtualPeerBinding {
    let readFile: URL
    let arguments: [String]

    static func resolve(binding: [String: Any], capabilities: [String: Any], qemuSHA256: String? = nil,
                        profile: String, apSHA256: String? = nil, session: URL, run: URL) throws -> Self? {
        guard let enabled = binding["virtualPeerEnabled"] else { return nil }
        guard let number = enabled as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else {
            throw SimulatorError.transport("虚拟通信启用标志无效")
        }
        guard number.boolValue else { return nil }
        let layouts = try validatedLayouts(capabilities: capabilities, qemuSHA256: qemuSHA256)
        _ = try StandaloneNorBinding.boundLayout(binding: binding, layouts: layouts, profile: profile, apSHA256: apSHA256)
        guard let id = binding["instanceID"] as? String, UUID(uuidString: id)?.uuidString.lowercased() == id,
              integer(binding["virtualPeerStateSchema"]) == 1,
              let digest = binding["virtualPeerStateSHA256"] as? String, FirmwareLibraryValidation.digest(digest),
              run.deletingLastPathComponent() == session, StandaloneBrokerConfiguration.validRun(run.lastPathComponent) else {
            throw SimulatorError.transport("虚拟通信身份与实例或核心不匹配")
        }
        let file = session.appendingPathComponent("instance-data/virtual-peer-state.json")
        guard binding["virtualPeerStatePath"] as? String == file.path else {
            throw SimulatorError.unsafeFile("虚拟身份不属于当前实例")
        }
        _ = try StandaloneSandboxFiles.directory(file.deletingLastPathComponent(), privateOnly: true)
        let data = try StandaloneSandboxFiles.regular(file, limit: 4_096, privateOnly: true)
        guard !data.isEmpty, StandaloneSandboxFiles.digest(data) == digest else {
            throw SimulatorError.unsafeFile("虚拟身份文件在启动前发生变化")
        }
        let output = run.appendingPathComponent("virtual-peer-state.pending.json")
        var info = stat()
        guard lstat(output.path, &info) != 0, errno == ENOENT else {
            throw SimulatorError.unsafeFile("虚拟身份输出已存在")
        }
        return Self(readFile: file, arguments: ["--virtual-peer-state", file.path,
            "--virtual-peer-state-sha256", digest, "--virtual-peer-output", output.path])
    }

    /// Use the same capability check before creating a synthetic key and before
    /// granting the state input. Neither path permits family/version fallback.
    static func validatedLayouts(capabilities: [String: Any], qemuSHA256: String? = nil) throws -> [String: StandaloneNorBinding.Layout] {
        let layouts = try StandaloneNorBinding.validatedLayouts(capabilities: capabilities, qemuSHA256: qemuSHA256)
        let commands = ["status", "connect", "inventory", "install", "watchface-select", "operation-status"]
        guard let cap = capabilities["virtualPeer"] as? [String: Any],
              Set(cap.keys) == ["schema", "runnerProtocol", "controlProtocol", "transport", "qemuSHA256",
                "profiles", "stateSchema", "automaticConnect", "physicalRadio",
                "maxRequestBytes", "maxResponseBytes", "commands"],
              integer(cap["schema"]) == 1, integer(cap["runnerProtocol"]) == 1,
              integer(cap["controlProtocol"]) == 1, integer(cap["stateSchema"]) == 1,
              integer(cap["maxRequestBytes"]) == 2_048, integer(cap["maxResponseBytes"]) == 65_536,
              cap["transport"] as? String == "local-virtual-hci",
              let profiles = cap["profiles"] as? [String: Any], Set(profiles.keys) == Set(layouts.keys),
              profiles.allSatisfy({ key, value in
                  guard let descriptor = value as? [String: Any], Set(descriptor.keys) == ["apSHA256"] else { return false }
                  return descriptor["apSHA256"] is String
              }),
              boolean(cap["automaticConnect"]) == false, boolean(cap["physicalRadio"]) == false,
              cap["commands"] as? [String] == commands else {
            throw SimulatorError.transport("虚拟通信能力与 NOR 固件描述不匹配")
        }
        return layouts
    }

    private static func boolean(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return nil }
        return number.boolValue
    }
    private static func integer(_ value: Any?) -> Int? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
        switch String(cString: number.objCType) {
        case "c", "s", "i", "l", "q": return Int(exactly: number.int64Value)
        case "C", "S", "I", "L", "Q": return Int(exactly: number.uint64Value)
        default: return nil
        }
    }
}
