import CoreFoundation
import Darwin
import Foundation

/// Native-side validation of the optional, exact-profile NOR working file.
/// This grants one private file, never a firmware image or an instance directory.
struct StandaloneNorBinding {
    let writeFile: URL
    let arguments: [String]

    struct Layout {
        let apSHA256: String
        let apBytes: Int
        let apOffset: Int
        let flashBytes: Int
        let offset: Int
        let byteCount: Int
        let factoryOffset: Int
        let configOffset: Int?
        let configByteCount: Int?
    }

    static func resolve(binding: [String: Any], capabilities: [String: Any],
                        qemuSHA256: String? = nil, profile: String, apSHA256: String? = nil,
                        session: URL) throws -> Self? {
        let keys = ["norImage", "norSHA256", "norOffset", "norBytes", "norInitialize",
                    "norAPSHA256", "norAPBytes", "norAPOffset", "norFlashBytes", "norFactoryOffset",
                    "norConfigOffset", "norConfigBytes"]
        let enabled = boolean(binding["norPersistent"])
        guard enabled == true || keys.contains(where: { binding[$0] != nil }) else {
            if binding["norPersistent"] != nil && enabled != false {
                throw SimulatorError.transport("NOR 持久化标志无效")
            }
            return nil
        }
        guard enabled == true,
              let initialize = boolean(binding["norInitialize"]),
              let digest = binding["norSHA256"] as? String, FirmwareLibraryValidation.digest(digest) else {
            throw SimulatorError.transport("NOR 数据布局与已核验核心或固件不匹配")
        }
        let layouts = try validatedLayouts(capabilities: capabilities, qemuSHA256: qemuSHA256)
        let layout = try boundLayout(binding: binding, layouts: layouts, profile: profile, apSHA256: apSHA256)
        let root = session.appendingPathComponent("instance-data", isDirectory: true)
        _ = try StandaloneSandboxFiles.directory(root, privateOnly: true)
        let file = root.appendingPathComponent("nor-config.bin")
        var info = stat()
        guard binding["norImage"] as? String == file.path,
              lstat(file.path, &info) == 0, info.st_uid == geteuid(),
              info.st_mode & 0o777 == 0o600, info.st_mode & S_IFMT == S_IFREG, info.st_nlink == 1 else {
            throw SimulatorError.unsafeFile("NOR 工作盘属性无效")
        }
        // Preparation and commit own snapshot integrity. Launch validates the
        // bounded working file without re-reading the entire image.
        try StandaloneSandboxFiles.validateRegular(file, expectedBytes: layout.byteCount, limit: layout.byteCount)
        return Self(writeFile: file, arguments: ["--instance-nor-image", file.path,
            "--instance-nor-sha256", digest] + (initialize ? ["--instance-nor-initialize"] : []))
    }

    /// Validate the packaged capability geometry. Legacy digest parameters are
    /// descriptive only and never authorize a particular executable or AP.
    static func validatedLayouts(capabilities: [String: Any], qemuSHA256: String? = nil) throws -> [String: Layout] {
        guard let capability = capabilities["norPersistence"] as? [String: Any],
              Set(capability.keys) == ["schema", "runnerProtocol", "qemuSHA256", "profiles"],
              integer(capability["schema"]) == 1, integer(capability["runnerProtocol"]) == 1,
              let profiles = capability["profiles"] as? [String: Any], !profiles.isEmpty else {
            throw SimulatorError.transport("NOR 能力描述无效")
        }
        var layouts: [String: Layout] = [:]
        for (profile, value) in profiles {
            let required: Set<String> = ["apSHA256", "apBytes", "apOffset", "flashBytes", "offset", "bytes", "factoryOffset"]
            guard StandaloneRunLimits.validProfileID(profile), !profile.contains(where: { $0.isWhitespace }),
                  let descriptor = value as? [String: Any],
                  Set(descriptor.keys) == required || Set(descriptor.keys) == required.union(["configOffset", "configBytes"]),
                  let apSHA256 = descriptor["apSHA256"] as? String, FirmwareLibraryValidation.digest(apSHA256),
                  let apBytes = integer(descriptor["apBytes"]), apBytes > 0,
                  let flashBytes = integer(descriptor["flashBytes"]), flashBytes > 0, flashBytes <= 16 * 1_024 * 1_024,
                  let apOffset = integer(descriptor["apOffset"]), apOffset >= 0, apOffset <= flashBytes,
                  apBytes <= flashBytes - apOffset,
                  let offset = integer(descriptor["offset"]), offset >= apBytes, offset < flashBytes,
                  apOffset + apBytes <= offset,
                  let byteCount = integer(descriptor["bytes"]), byteCount > 0, byteCount <= flashBytes,
                  let factoryOffset = integer(descriptor["factoryOffset"]),
                  offset.isMultiple(of: 512), byteCount.isMultiple(of: 512), factoryOffset.isMultiple(of: 512),
                  factoryOffset >= offset, factoryOffset < flashBytes else {
                throw SimulatorError.transport("NOR 固件与数据布局描述无效")
            }
            var configOffset: Int?, configByteCount: Int?
            if descriptor["configOffset"] != nil {
                guard let start = integer(descriptor["configOffset"]), start >= 0, start < offset,
                      let count = integer(descriptor["configBytes"]), count > 0, count < byteCount,
                      start.isMultiple(of: 512), count.isMultiple(of: 512), count <= offset - start,
                      start + count <= apOffset || start >= apOffset + apBytes else {
                    throw SimulatorError.transport("NOR 配置分段与固件或尾部重叠")
                }
                configOffset = start; configByteCount = count
            }
            // Split images contain the complete tail first, then config. A
            // single-extent descriptor remains bounded by its declared end.
            // Gaps between extents never become writable.
            let tailByteCount = byteCount - (configByteCount ?? 0)
            guard tailByteCount <= flashBytes - offset,
                  configByteCount == nil || tailByteCount == flashBytes - offset,
                  factoryOffset < offset + tailByteCount else {
                throw SimulatorError.transport("NOR 工作盘长度与分段范围不匹配")
            }
            layouts[profile] = Layout(apSHA256: apSHA256, apBytes: apBytes, apOffset: apOffset, flashBytes: flashBytes,
                offset: offset, byteCount: byteCount, factoryOffset: factoryOffset,
                configOffset: configOffset, configByteCount: configByteCount)
        }
        return layouts
    }

    /// Bind the exact profile and working geometry; declared AP/core digests are
    /// release metadata and do not authorize execution. No family fallback.
    static func boundLayout(binding: [String: Any], layouts: [String: Layout],
                            profile: String, apSHA256: String?) throws -> Layout {
        guard boolean(binding["norPersistent"]) == true,
              let layout = layouts[profile],
              binding["profileID"] as? String == profile,
              integer(binding["norAPBytes"]) == layout.apBytes,
              integer(binding["norAPOffset"]) == layout.apOffset,
              integer(binding["norFlashBytes"]) == layout.flashBytes,
              integer(binding["norOffset"]) == layout.offset,
              integer(binding["norBytes"]) == layout.byteCount,
              integer(binding["norFactoryOffset"]) == layout.factoryOffset else {
            throw SimulatorError.transport("NOR 数据布局与已核验核心或固件不匹配")
        }
        if let offset = layout.configOffset, let byteCount = layout.configByteCount {
            guard integer(binding["norConfigOffset"]) == offset, integer(binding["norConfigBytes"]) == byteCount else {
                throw SimulatorError.transport("NOR 配置分段与启动请求不匹配")
            }
        } else if binding["norConfigOffset"] != nil || binding["norConfigBytes"] != nil {
            throw SimulatorError.transport("NOR 启动请求包含未声明的分段")
        }
        return layout
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
