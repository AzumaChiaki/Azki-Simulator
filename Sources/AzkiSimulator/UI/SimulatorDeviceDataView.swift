import SwiftUI

/// Every data input of the instance in one table: the wearer (on the wrist,
/// pulse, cadence, walking) and the board (battery, charger, temperature).
/// While the device runs, "应用" changes what the emulated hardware senses at
/// once (live-set) and keeps the value for the next boot; when it is stopped,
/// the value is saved for the next boot. Rows appear only for inputs that
/// really drive this preset's firmware; the firmware itself computes what it
/// shows from the raw sensor and battery data.
@MainActor struct SimulatorDeviceDataView: View {
    let runtime: StandaloneRuntimeModel
    let session: SimulatorSessionModel
    let enabled: Bool
    let editSystemSettings: () -> Void

    @State private var settings: InstanceFirmwareSettings?
    @State private var batteryInputs: InstanceFirmwareSettings.BatteryInputs?
    @State private var healthInputs: InstanceFirmwareSettings.HealthInputs?
    @State private var live: SimulatorLiveState?
    @State private var liveUnavailable: String?

    @State private var worn = true
    @State private var heartRateText = ""
    @State private var cadenceText = ""
    @State private var walkText = "100"
    @State private var bootWalkText = ""
    @State private var levelText = ""
    @State private var charger: InstanceFirmwareSettings.Charger = .auto
    @State private var temperatureText = ""

    @State private var busy = false
    @State private var message: String?
    @State private var messageIsError = false

    private var liveContext: String {
        "\(session.manifest?.id.uuidString ?? "none")/\(session.isRunning)/\(session.status?["generation"].map { "\($0)" } ?? "")"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if settings == nil && runtime.instanceID != nil {
                ProgressView().controlSize(.small).accessibilityLabel("正在读取设备数据")
            } else if !hasRows {
                note(runtime.instanceID == nil
                     ? "设备数据只适用于设备实例。"
                     : "此型号的传感器与电池输入尚未验证能驱动固件，因此不提供修改。")
            } else {
                table
            }
            if let liveUnavailable, session.isRunning { note(liveUnavailable) }
            if let message {
                Text(message).font(.system(size: 12)).foregroundStyle(messageIsError ? .red : SimulatorTheme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            note(footer)
            HStack(spacing: 8) {
                Text("语言、时区、时间格式、开机时间和系统属性：").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Button("系统设置…", action: editSystemSettings).disabled(!enabled || runtime.instanceID == nil)
                    .accessibilityIdentifier("simulator.firmware-settings")
            }
        }
        .task(id: runtime.instanceID) { await loadSettings() }
        .task(id: liveContext) { await pollLive() }
    }

    // MARK: - Table

    private var showsWear: Bool { healthInputs?.heartRate == true || live?.supports("heartRate") == true }
    private var showsSteps: Bool { healthInputs?.steps == true || live?.supports("steps") == true }
    private var showsLevel: Bool { batteryInputs?.level == true || live?.supports("batteryLevel") == true }
    private var showsCharger: Bool { batteryInputs?.charger == true || live?.supports("charger") == true }
    private var showsTemperature: Bool { live?.supports("temperature") == true }
    private var hasRows: Bool { showsWear || showsSteps || showsLevel || showsCharger || showsTemperature }
    private var running: Bool { session.isRunning && live != nil }

    private var table: some View {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
            GridRow {
                header("项目"); header("当前"); header("设定"); header("")
            }
            Divider().gridCellUnsizedAxes(.horizontal)
            if showsWear || showsSteps { group("佩戴与健康") }
            if showsWear {
                GridRow {
                    label("佩戴")
                    current(live?.bool("worn").map { $0 ? "佩戴中" : "未佩戴" } ?? bootWorn)
                    Picker("佩戴", selection: $worn) {
                        Text("佩戴").tag(true); Text("未佩戴").tag(false)
                    }.pickerStyle(.segmented).labelsHidden().frame(width: 150)
                    applyButton("应用", id: "worn", enabled: true) { await applyWorn() }
                }
                GridRow {
                    label("心率")
                    current(live?.int("heartRate").map { "\($0) 次/分" } ?? bootHeartRate)
                    field($heartRateText, unit: "次/分", label: "心率，每分钟次数")
                    applyButton("应用", id: "heart-rate", enabled: heartRate != nil) { await applyHeartRate() }
                }
            }
            if showsSteps {
                GridRow {
                    label("步频")
                    current(live?.int("cadence").map { "\($0) 步/分" } ?? settings?.health?.cadence.map { "\($0) 步/分（开机）" } ?? "110 步/分")
                    field($cadenceText, unit: "步/分", label: "步频，每分钟步数")
                    applyButton("应用", id: "cadence", enabled: cadence != nil) { await applyCadence() }
                }
                GridRow {
                    label("走路")
                    current(walkSummary)
                    field($walkText, unit: "步", label: "立即步行的步数")
                    applyButton("走", id: "walk", enabled: running && live?.supports("steps") == true
                                && parse(walkText, SimulatorLiveState.addStepsRange) != nil) { await applyWalk() }
                }
                GridRow {
                    label("开机后步行")
                    current(settings?.health?.walkSteps.map { "\($0) 步" } ?? "不走")
                    field($bootWalkText, unit: "步", label: "每次开机后步行的步数")
                    applyButton("保存", id: "boot-walk", enabled: bootWalk != nil) { await applyBootWalk() }
                }
            }
            if showsLevel || showsCharger || showsTemperature { group("电池") }
            if showsLevel {
                GridRow {
                    label("电量")
                    current(settings?.battery?.level.map { "开机 \($0)%" } ?? "固件默认")
                    field($levelText, unit: "%", label: "电池电量百分比")
                    applyButton("应用", id: "battery-level", enabled: level != nil) { await applyLevel() }
                }
            }
            if showsCharger {
                GridRow {
                    label("充电")
                    current(live?.charger?.title ?? chargerTitle(settings?.battery?.charger ?? .auto))
                    Picker("充电", selection: $charger) {
                        Text("未接充电器").tag(InstanceFirmwareSettings.Charger.unplugged)
                        Text("充电中").tag(InstanceFirmwareSettings.Charger.charging)
                        Text("已充满").tag(InstanceFirmwareSettings.Charger.full)
                    }.labelsHidden().frame(width: 150)
                    applyButton("应用", id: "charger", enabled: charger != .auto) { await applyCharger() }
                }
            }
            if showsTemperature {
                GridRow {
                    label("电池温度")
                    current(live?.number("temperatureC").map { String(format: "%.1f °C", $0) } ?? "—")
                    field($temperatureText, unit: "°C", label: "电池温度，摄氏度")
                    applyButton("应用", id: "temperature", enabled: running && temperature != nil) { await applyTemperature() }
                }
            }
        }
    }

    private var bootWorn: String { settings?.health?.worn == false ? "未佩戴（开机）" : "佩戴（开机）" }
    private var bootHeartRate: String {
        "\(settings?.health?.heartRate ?? InstanceFirmwareSettings.restingHeartRate) 次/分（开机）"
    }
    private var walkSummary: String {
        guard let live, live.supports("steps") else { return session.isRunning ? "—" : "未运行" }
        var text = "已走 \(live.int("walked") ?? 0) 步"
        if live.stepsRemaining > 0 { text += "，还剩 \(live.stepsRemaining)" }
        return text
    }

    private var footer: String {
        var lines = [session.isRunning
            ? "设备运行中：“应用”立即改变模拟硬件的输入，并保存为下次开机的值；“保存”只作用于下次开机。"
            : "设备未运行：修改保存为下次开机的值；运行后可在此立即修改。"]
        lines.append("步数、心率、电量都由固件自己的算法从模拟的传感器和电池数据算出：心率可能相差 1–3 次/分，未佩戴时固件提示佩戴；运行中改电量只改变电池电压，固件电量计逐步追随。")
        if showsSteps { lines.append("开机后步行在固件计步服务就绪（约 75 秒）后开始，起步的几步用于识别步行。") }
        if let note = levelNote { lines.append(note) }
        return lines.joined(separator: "\n")
    }

    private var levelNote: String? {
        guard showsLevel, let model = runtime.modelConstraint else { return nil }
        if model.hasPrefix("miwear.watch.q66") {
            return "手环 11 冷启动只按电池电压估算电量，电压 7 mV 一档，少数设定值会显示相差 1%。"
        }
        return nil
    }

    // MARK: - Pieces

    private func header(_ text: String) -> some View {
        Text(text).font(.system(size: 11, weight: .semibold)).foregroundStyle(SimulatorTheme.secondary)
    }

    private func group(_ title: String) -> some View {
        GridRow {
            Text(title).fontWeight(.medium).gridCellColumns(4).padding(.top, 4)
        }
    }

    private func label(_ text: String) -> some View { Text(text).frame(minWidth: 72, alignment: .leading) }

    private func current(_ text: String) -> some View {
        Text(text).font(.system(size: 12, design: .monospaced)).foregroundStyle(SimulatorTheme.secondary)
            .frame(minWidth: 130, alignment: .leading)
    }

    private func field(_ text: Binding<String>, unit: String, label: String) -> some View {
        HStack(spacing: 4) {
            TextField("", text: text).textFieldStyle(.roundedBorder).frame(width: 80).accessibilityLabel(label)
            Text(unit).font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
        }
    }

    private func applyButton(_ title: String, id: String, enabled: Bool, _ run: @escaping () async -> Void) -> some View {
        Button(title) { Task { busy = true; await run(); busy = false } }
            .disabled(!self.enabled || busy || !enabled)
            .accessibilityIdentifier("simulator.data.\(id)")
    }

    private func note(_ text: String) -> some View {
        Text(text).font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func chargerTitle(_ value: InstanceFirmwareSettings.Charger) -> String {
        switch value {
        case .auto, .unplugged: "未接充电器（开机）"
        case .charging: "充电中（开机）"
        case .full: "已充满（开机）"
        }
    }

    private func parse(_ text: String, _ range: ClosedRange<Int>) -> Int? {
        Int(text.trimmingCharacters(in: .whitespaces)).flatMap { range.contains($0) ? $0 : nil }
    }

    private var heartRate: Int? { parse(heartRateText, InstanceFirmwareSettings.heartRateRange) }
    private var cadence: Int? { parse(cadenceText, InstanceFirmwareSettings.cadenceRange) }
    private var bootWalk: Int? { parse(bootWalkText, 0...InstanceFirmwareSettings.maximumSteps) }
    private var level: Int? { parse(levelText, SimulatorLiveState.batteryLevelRange) }
    private var temperature: Double? {
        Double(temperatureText.trimmingCharacters(in: .whitespaces)).flatMap { $0.isFinite && (-40...85).contains($0) ? $0 : nil }
    }

    // MARK: - Apply

    private func applyWorn() async {
        await apply(live: live?.supports("heartRate") == true ? ["worn": .bool(worn)] : nil) { $0.health(\.worn, worn ? nil : false) }
    }

    private func applyHeartRate() async {
        guard let heartRate else { return }
        await apply(live: live?.supports("heartRate") == true ? ["heartRate": .number(Double(heartRate))] : nil) {
            $0.health(\.heartRate, heartRate)
        }
    }

    private func applyCadence() async {
        guard let cadence else { return }
        await apply(live: live?.supports("steps") == true ? ["cadence": .number(Double(cadence))] : nil) {
            $0.health(\.cadence, cadence)
        }
    }

    private func applyWalk() async {
        guard let steps = parse(walkText, SimulatorLiveState.addStepsRange) else { return }
        await apply(live: ["addSteps": .number(Double(steps))], boot: nil)
    }

    private func applyBootWalk() async {
        guard let steps = bootWalk else { return }
        await apply(live: nil) { $0.health(\.walkSteps, steps == 0 ? nil : steps) }
    }

    private func applyLevel() async {
        guard let level else { return }
        await apply(live: live?.supports("batteryLevel") == true ? ["batteryLevel": .number(Double(level))] : nil) {
            $0.battery { $0.level = level }
        }
    }

    private func applyCharger() async {
        await apply(live: live?.supports("charger") == true ? ["charger": .string(charger.rawValue)] : nil) {
            $0.battery { $0.charger = charger }
        }
    }

    private func applyTemperature() async {
        guard let temperature else { return }
        await apply(live: ["temperatureC": .number(temperature)], boot: nil)
    }

    /// Live change first (only while running), then the next-boot value.
    private func apply(live change: [String: SimulatorJSON]?,
                       boot: ((inout InstanceFirmwareSettings) -> Void)?) async {
        var done: [String] = []
        do {
            if let change, session.isRunning, live != nil {
                receive(try await session.liveCommand(SimulatorLiveCommands.set, arguments: change))
                done.append("已立即生效")
            }
            if let boot, runtime.instanceID != nil {
                var next = try await currentSettings()
                boot(&next)
                adopt(try await runtime.saveFirmwareSettings(next))
                done.append("已保存为下次开机的值")
            }
            message = done.isEmpty ? nil : done.joined(separator: "，")
            messageIsError = false
        } catch {
            let text = error.localizedDescription
            message = text.contains("[live_outcome_unknown]")
                ? "写入可能已生效但未得到确认，请看设备画面或等待刷新，不要重复提交。" : text
            messageIsError = true
        }
    }

    private func apply(live change: [String: SimulatorJSON]?,
                       _ boot: @escaping (inout InstanceFirmwareSettings) -> Void) async {
        await apply(live: change, boot: boot)
    }

    // MARK: - State

    /// Re-read before every save so changes made in the system settings sheet
    /// are kept.
    private func currentSettings() async throws -> InstanceFirmwareSettings {
        let receipt = try await runtime.firmwareSettings()
        adopt(receipt)
        return receipt.settings
    }

    private func adopt(_ receipt: InstanceFirmwareSettingsReceipt) {
        let first = settings == nil
        settings = receipt.settings
        batteryInputs = receipt.batteryInputs(for: runtime.modelConstraint)
        healthInputs = receipt.healthInputs(for: runtime.modelConstraint)
        guard first else { return }
        let health = receipt.settings.health
        worn = health?.worn ?? true
        heartRateText = String(health?.heartRate ?? InstanceFirmwareSettings.restingHeartRate)
        cadenceText = String(health?.cadence ?? 110)
        bootWalkText = String(health?.walkSteps ?? 0)
        levelText = receipt.settings.battery?.level.map(String.init) ?? ""
        let boot = receipt.settings.battery?.charger ?? .auto
        charger = boot == .auto ? .unplugged : boot
    }

    private func loadSettings() async {
        settings = nil; batteryInputs = nil; healthInputs = nil
        guard runtime.instanceID != nil else { return }
        do { adopt(try await runtime.firmwareSettings()) }
        catch { message = error.localizedDescription; messageIsError = true; settings = InstanceFirmwareSettings() }
    }

    private func receive(_ reply: SimulatorJSON) {
        guard let next = SimulatorLiveState(reply) else { return }
        let first = live == nil
        live = next; liveUnavailable = nil
        guard first else { return }
        if let value = next.bool("worn") { worn = value }
        if let value = next.int("heartRate"), value > 0 { heartRateText = String(value) }
        if let value = next.int("cadence") { cadenceText = String(value) }
        if let value = next.charger, let mapped = InstanceFirmwareSettings.Charger(rawValue: value.rawValue) { charger = mapped }
        if let value = next.number("temperatureC") { temperatureText = String(format: "%.1f", value) }
    }

    /// Refreshes while the device runs: faster while steps are still being walked.
    private func pollLive() async {
        live = nil; liveUnavailable = nil
        guard session.isRunning else { return }
        while !Task.isCancelled, session.isRunning {
            if !busy {
                do { receive(try await session.liveCommand(SimulatorLiveCommands.get)) }
                catch {
                    if live == nil {
                        let text = error.localizedDescription
                        liveUnavailable = text.contains("[unsupported") || text.contains("不受支持")
                            ? "当前运行的核心不提供实时输入，修改将在下次开机生效。" : "暂时读不到实时输入：\(text)"
                    }
                }
            }
            let walking = (live?.stepsRemaining ?? 0) > 0
            do { try await Task.sleep(for: .seconds(walking ? 1 : 3)) } catch { break }
        }
    }
}

private extension InstanceFirmwareSettings {
    mutating func health<Value>(_ key: WritableKeyPath<Health, Value?>, _ value: Value?) {
        var next = health ?? Health()
        next[keyPath: key] = value
        health = next.isEmpty ? nil : next
    }

    mutating func battery(_ change: (inout Battery) -> Void) {
        var next = battery ?? Battery()
        change(&next)
        battery = next
    }
}
