import SwiftUI

/// Instance firmware settings: written into the firmware's own persist.db at
/// the next boot. The firmware keeps any change made on the device itself
/// unless the same key is overridden here.
@MainActor struct SimulatorFirmwareSettingsSheet: View {
    let runtime: StandaloneRuntimeModel
    let restartAvailable: Bool
    let restart: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var settings = InstanceFirmwareSettings()
    @State private var observed: [String: String] = [:]
    @State private var observedAt: Date?
    @State private var loaded = false
    @State private var loadFailed = false
    @State private var saving = false
    @State private var message: String?
    @State private var customTime = Date()
    @State private var newKey = ""
    @State private var newValue = ""
    @State private var filter = ""

    private enum Choice: Hashable { case keep, value(String), reset }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("固件设置").font(.title2)
            Text("下次开机时写入固件的系统设置数据库（persist.db），并按以下时钟启动；佩戴、心率、步行、电量、充电在工作区“设备数据”中修改。目录覆盖的手环 9 / 9 Pro / 10 / 10 Pro / 11 与导入固件共用同一套 /data 存储，均按实例的硬件预设生效。")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if !loaded {
                ProgressView().controlSize(.small)
            } else {
                Form {
                    clockSection
                    Section("系统") {
                        choicePicker("语言", key: InstanceFirmwareSettingKeys.locale,
                                     options: InstanceFirmwareSettingKeys.languages)
                        choicePicker("时间格式", key: InstanceFirmwareSettingKeys.timeFormat,
                                     options: [("false", "24 小时制"), ("true", "12 小时制")])
                        timeZonePicker
                    }
                    propertiesSection
                }
                .formStyle(.grouped)
                .frame(minHeight: 420)
            }
            if let message {
                Text(message).font(.callout).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            }
            if loadFailed {
                HStack {
                    Text("未能读取已保存的设置；可重试，或直接保存以写入新的设置。")
                        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    Button("重新读取") { Task { await load() } }
                }
            }
            HStack {
                if let observedAt {
                    Text("固件当前值读取于 \(observedAt.formatted(date: .abbreviated, time: .shortened))")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                if restartAvailable {
                    Button("保存并重新开机") { save(thenRestart: true) }.disabled(!loaded || saving)
                }
                Button("保存") { save(thenRestart: false) }.keyboardShortcut(.defaultAction).disabled(!loaded || saving)
            }
        }
        .padding(24).frame(width: 620)
        .task { await load() }
    }

    private var clockSection: some View {
        Section("时间") {
            Picker("开机时间", selection: Binding(get: { settings.clock.mode }, set: { mode in
                settings.clock = .init(mode: mode, offsetSeconds: mode == .offset ? Int(customTime.timeIntervalSinceNow.rounded()) : nil)
            })) {
                Text("跟随 Mac 时间").tag(InstanceFirmwareSettings.ClockMode.host)
                Text("自定义时间").tag(InstanceFirmwareSettings.ClockMode.offset)
                Text("保持固件自己的时间").tag(InstanceFirmwareSettings.ClockMode.firmware)
            }
            if settings.clock.mode == .offset {
                DatePicker("设备时间", selection: $customTime, displayedComponents: [.date, .hourAndMinute])
                Text("每次开机都按“现在 + 相同偏移”计算，设备时间随 Mac 一起走。")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var timeZonePicker: some View {
        let key = InstanceFirmwareSettingKeys.timeZone
        let mac = TimeZone.current.identifier
        var zones = InstanceFirmwareSettingKeys.timeZones
        if case let .some(.some(value)) = settings.override(key), !zones.contains(value) { zones.insert(value, at: 0) }
        return Picker("时区", selection: binding(key)) {
            Text(settings.clock.mode == .host ? "跟随 Mac（\(mac)）" : "不修改").tag(Choice.keep)
            ForEach(zones, id: \.self) { Text($0).tag(Choice.value($0)) }
            Text("恢复固件默认").tag(Choice.reset)
        }
    }

    private var propertiesSection: some View {
        Section {
            TextField("筛选属性", text: $filter)
            ForEach(rowKeys, id: \.self) { key in
                propertyRow(key)
            }
            HStack {
                TextField("persist.新属性", text: $newKey).frame(minWidth: 200)
                TextField("值", text: $newValue)
                Button("添加") {
                    let key = newKey.trimmingCharacters(in: .whitespaces)
                    guard InstanceFirmwareSettings.validKey(key), InstanceFirmwareSettings.validValue(newValue) else {
                        message = "属性名须以 persist. 开头，只含字母、数字、点、下划线或连字符；值不能含换行或制表符。"
                        return
                    }
                    settings.set(key, .some(newValue)); newKey = ""; newValue = ""; message = nil
                }
            }
        } header: {
            Text("全部属性（\(observed.count) 项来自上次运行）")
        } footer: {
            Text("留空表示保持固件当前值；“恢复默认”会删除该项，让固件使用内置默认值。")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var rowKeys: [String] {
        let keys = Set(observed.keys).union(settings.properties.keys)
            .subtracting(["persist.rtc_save"])
        let needle = filter.trimmingCharacters(in: .whitespaces).lowercased()
        return keys.filter { needle.isEmpty || $0.lowercased().contains(needle) }.sorted()
    }

    private func propertyRow(_ key: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text(key).font(.system(size: 12, design: .monospaced)).textSelection(.enabled)
                Text(observed[key].map { "固件当前：\($0)" } ?? "固件当前：未设置")
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            switch settings.override(key) {
            case .none:
                Button("修改") { settings.set(key, .some(observed[key] ?? "")) }
            case .some(.none):
                Text("将恢复默认").font(.caption)
                Button("撤销") { settings.set(key, nil) }
            case .some(.some(let value)):
                TextField("值", text: Binding(get: { value }, set: { settings.set(key, .some($0)) }))
                    .frame(width: 180)
                Button("撤销") { settings.set(key, nil) }
            }
            if !isReset(key) {
                Button("恢复默认") { settings.set(key, .some(nil)) }
            }
        }
    }

    private func isReset(_ key: String) -> Bool {
        if case .some(.none) = settings.override(key) { return true }
        return false
    }

    private func choicePicker(_ title: String, key: String, options: [(String, String)]) -> some View {
        Picker(title, selection: binding(key)) {
            Text(observed[key].map { "不修改（当前 \($0)）" } ?? "不修改").tag(Choice.keep)
            ForEach(options, id: \.0) { Text($0.1).tag(Choice.value($0.0)) }
            if case let .some(.some(value)) = settings.override(key), !options.contains(where: { $0.0 == value }) {
                Text(value).tag(Choice.value(value))
            }
            Text("恢复固件默认").tag(Choice.reset)
        }
    }

    private func binding(_ key: String) -> Binding<Choice> {
        Binding(get: {
            switch settings.override(key) {
            case .none: return .keep
            case .some(.none): return .reset
            case .some(.some(let value)): return .value(value)
            }
        }, set: { choice in
            switch choice {
            case .keep: settings.set(key, nil)
            case .reset: settings.set(key, .some(nil))
            case .value(let value): settings.set(key, .some(value))
            }
        })
    }

    private func apply(_ receipt: InstanceFirmwareSettingsReceipt) {
        settings = receipt.settings
        observed = receipt.observed?.properties ?? [:]
        observedAt = receipt.observed?.recordedAtUnix.map { Date(timeIntervalSince1970: TimeInterval($0)) }
        if settings.clock.mode == .offset, let offset = settings.clock.offsetSeconds {
            customTime = Date(timeIntervalSinceNow: TimeInterval(offset))
        }
        loaded = true
    }

    private func load() async {
        message = nil
        do { apply(try await runtime.firmwareSettings()); loadFailed = false }
        catch { message = error.localizedDescription; loadFailed = true; loaded = true }
    }

    private func save(thenRestart: Bool) {
        var candidate = settings
        if let bad = candidate.properties.first(where: { !InstanceFirmwareSettings.validKey($0.key)
            || !($0.value.map(InstanceFirmwareSettings.validValue) ?? true) }) {
            message = "属性 \(bad.key) 的值不能含换行或制表符，且不超过 1024 个字符。"; return
        }
        if candidate.clock.mode == .offset {
            let offset = Int(customTime.timeIntervalSinceNow.rounded())
            guard abs(offset) <= InstanceFirmwareSettings.maximumOffsetSeconds else {
                message = "自定义时间超出可设置范围"; return
            }
            candidate.clock.offsetSeconds = offset
        }
        saving = true
        Task {
            defer { saving = false }
            do {
                apply(try await runtime.saveFirmwareSettings(candidate))
                dismiss()
                if thenRestart { restart() }
            } catch { message = error.localizedDescription }
        }
    }
}
