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
    @State private var batteryInputs: InstanceFirmwareSettings.BatteryInputs?
    @State private var healthInputs: InstanceFirmwareSettings.HealthInputs?

    private enum Choice: Hashable { case keep, value(String), reset }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("固件设置").font(.title2)
            Text("下次开机时写入固件的系统设置数据库（persist.db），并按以下时钟启动。目录覆盖的手环 9 / 9 Pro / 10 / 10 Pro / 11 与导入固件共用同一套 /data 存储，均按实例的硬件预设生效。")
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
                    batterySection
                    healthSection
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

    /// Board inputs: the instance hardware preset decides which part exists, so
    /// every value here is written into the session and applied by the core.
    private var batterySection: some View {
        Section {
            if batteryInputs?.level == true {
                HStack {
                    Text("开机电量")
                    Spacer()
                    if let level = settings.battery?.level {
                        TextField("电量", text: levelBinding).frame(width: 56)
                            .multilineTextAlignment(.trailing)
                            .accessibilityLabel("开机电量百分比")
                        Stepper("%", value: Binding(get: { level },
                                                    set: { value in setBattery { $0.level = min(max(value, 0), 100) } }),
                                in: 0...100, step: 1)
                        Button("恢复默认") { setBattery { $0.level = nil } }
                    } else {
                        Text("固件默认").foregroundStyle(.secondary)
                        Button("设置") { setBattery { $0.level = 80 } }
                    }
                }
            }
            if batteryInputs?.charger == true {
                Picker("充电状态", selection: Binding(get: { settings.battery?.charger ?? .auto },
                                                     set: { choice in setBattery { $0.charger = choice } })) {
                    Text("跟随固件（未接充电器）").tag(InstanceFirmwareSettings.Charger.auto)
                    Text("未接充电器").tag(InstanceFirmwareSettings.Charger.unplugged)
                    Text("充电中").tag(InstanceFirmwareSettings.Charger.charging)
                    Text("已充满").tag(InstanceFirmwareSettings.Charger.full)
                }
            }
            DisclosureGroup("高级：电池电压采样原始值") {
                HStack {
                    TextField("电池采样原始值", text: sampleBinding).frame(width: 140)
                    Text("0…65535").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("恢复默认") { setBattery { $0.sampleRaw = nil } }
                }
            }
        } header: {
            Text("电池")
        } footer: {
            Text(batteryFooter)
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var batteryFooter: String {
        var lines: [String] = []
        if batteryInputs?.level == true {
            lines.append("开机电量在下次开机时生效，之后由固件自己的电量计计算和显示。" + Self.batteryLevelNote(model: runtime.modelConstraint))
        } else {
            lines.append("此型号的开机电量尚未验证能驱动固件的电量计，因此不提供百分比设置，固件显示的电量保持其默认值。")
        }
        if batteryInputs?.charger == true {
            lines.append("充电状态在开机约 8 秒后才接入：开机时就接着充电器会让固件进入充电重启流程。")
        }
        lines.append("采样原始值是固件电池电压通路的原始计数，填写后优先于开机电量换算出的值。")
        return lines.joined(separator: "\n")
    }

    /// How the boot level reaches each preset's gauge (verified per firmware).
    static func batteryLevelNote(model: String?) -> String {
        let model = model ?? ""
        if model.hasPrefix("miwear.watch.n66") || model.hasPrefix("miwear.watch.o66") {
            return "此型号的电量计恢复保存的容量，并按“容量 × 1.055”显示；设置会按此换算写入保存的容量和对应的电池电压，显示值与设定值一致。"
        }
        if model.hasPrefix("miwear.watch.n67") || model.hasPrefix("miwear.watch.p67") {
            return "此型号的电量计恢复保存的整数百分比，电池电压采样设为该电量对应的开路电压，显示值与设定值一致。"
        }
        if model.hasPrefix("miwear.watch.q66") || model == "q66" {
            return "此型号冷启动时只按电池电压估算电量（不读取保存的电量），电压按 7 mV 一档取整，约 18 个设定值会显示为相差 1%。"
        }
        return "电池电压采样设为该电量对应的开路电压。"
    }

    private var levelBinding: Binding<String> {
        Binding(get: { settings.battery?.level.map(String.init) ?? "" },
                set: { text in
                    guard let value = Int(text.trimmingCharacters(in: .whitespaces)), (0...100).contains(value) else { return }
                    setBattery { $0.level = value }
                })
    }

    private var healthSection: some View {
        Section {
            if healthInputs?.steps == true {
                HStack {
                    TextField("开机后步行步数", text: intBinding(\.walkSteps, 0...InstanceFirmwareSettings.maximumSteps))
                        .frame(width: 160)
                    Text("步").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("恢复默认") { setHealth { $0.walkSteps = nil; $0.cadence = nil } }
                }
                HStack {
                    TextField("步频", text: intBinding(\.cadence, InstanceFirmwareSettings.cadenceRange)).frame(width: 160)
                    Text("步/分钟（40…200）").font(.caption).foregroundStyle(.secondary)
                }
            }
            if healthInputs?.heartRate == true {
                HStack {
                    TextField("心率", text: intBinding(\.heartRate, 0...InstanceFirmwareSettings.maximumHeartRate))
                        .frame(width: 160)
                    Text("次/分（0 = 未佩戴）").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("恢复默认") { setHealth { $0.heartRate = nil } }
                }
            }
        } header: {
            Text("健康数据")
        } footer: {
            Text(healthFooter)
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var healthFooter: String {
        guard let healthInputs, healthInputs.steps || healthInputs.heartRate else {
            return "此型号的运动与光学传感器尚未模拟，固件不会从这里得到步数或心率，因此不提供设置。"
        }
        var lines = ["模拟核心按这里的数值产生运动传感器与光学传感器的原始数据，步数和心率由固件自己的算法计算并显示；下次开机生效。"]
        if healthInputs.steps {
            lines.append("开机步行在开机约 75 秒、固件计步服务就绪后才开始；起步的几步用于识别步行，可能不计入。运行中也可以在“硬件输入”的实时控制里追加步行。")
        }
        if healthInputs.heartRate {
            lines.append("心率在固件测量时读到设定脉搏（固件自身算法可能相差 1–3 次/分）；0 表示未佩戴。")
        }
        return lines.joined(separator: "\n")
    }

    private func setBattery(_ change: (inout InstanceFirmwareSettings.Battery) -> Void) {
        var battery = settings.battery ?? InstanceFirmwareSettings.Battery()
        change(&battery)
        settings.battery = battery
    }

    private func setHealth(_ change: (inout InstanceFirmwareSettings.Health) -> Void) {
        var health = settings.health ?? InstanceFirmwareSettings.Health()
        change(&health)
        settings.health = health.isEmpty ? nil : health
    }

    private var sampleBinding: Binding<String> {
        Binding(get: { settings.battery?.sampleRaw.map(String.init) ?? "" },
                set: { text in
                    let trimmed = text.trimmingCharacters(in: .whitespaces)
                    let value = trimmed.isEmpty ? nil : Int(trimmed)
                    setBattery { $0.sampleRaw = value.flatMap { (0...InstanceFirmwareSettings.maximumSampleRaw).contains($0) ? $0 : nil } }
                })
    }

    private func intBinding(_ field: WritableKeyPath<InstanceFirmwareSettings.Health, Int?>,
                            _ range: ClosedRange<Int>) -> Binding<String> {
        Binding(get: { settings.health?[keyPath: field].map(String.init) ?? "" },
                set: { text in
                    let trimmed = text.trimmingCharacters(in: .whitespaces)
                    let value = trimmed.isEmpty ? nil : Int(trimmed)
                    setHealth { $0[keyPath: field] = value.flatMap { range.contains($0) ? $0 : nil } }
                })
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
        batteryInputs = receipt.batteryInputs(for: runtime.modelConstraint)
        healthInputs = receipt.healthInputs(for: runtime.modelConstraint)
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
