import SwiftUI

/// Inputs changed while the device runs (live-get / live-set). Each control
/// appears only when the running core reports it supported for this preset;
/// the firmware itself turns the raw sensor and battery data into what it
/// shows, so the values here are the wearer's and the board's, not readings.
@MainActor struct SimulatorLiveControlsView: View {
    let session: SimulatorSessionModel
    let enabled: Bool
    @State private var live: SimulatorLiveState?
    @State private var unavailable: String?
    @State private var stepsText = "100"
    @State private var cadenceText = ""
    @State private var heartRateText = ""
    @State private var charger: SimulatorLiveCharger = .unplugged
    @State private var levelText = ""
    @State private var rawText = ""
    @State private var temperatureText = ""
    @State private var clockDate = Date()
    @State private var busy = false
    @State private var message: String?
    @State private var outcomeUnknown = false

    private var context: String {
        "\(session.manifest?.id.uuidString ?? "none")/\(session.isRunning)/\(session.status?["generation"].map { "\($0)" } ?? "")"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("实时输入").fontWeight(.medium)
            if !session.isRunning {
                note("设备运行后可在这里实时修改支持的输入，立即生效，不需要重新开机。语言、时区、时间格式和系统属性只在开机时读取，请在“固件设置”中修改后重新开机。")
            } else if let live {
                controls(live)
            } else if let unavailable {
                note(unavailable)
            } else {
                ProgressView().controlSize(.small).accessibilityLabel("正在读取实时输入")
            }
            if let message {
                Text(message).font(.system(size: 12)).foregroundStyle(outcomeUnknown ? .orange : .red)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .task(id: context) { await poll() }
    }

    @ViewBuilder private func controls(_ live: SimulatorLiveState) -> some View {
        // Wearer: steps, cadence, pulse.
        if live.supports("steps") {
            Text(stepsSummary(live)).font(.system(size: 12, design: .monospaced))
                .foregroundStyle(SimulatorTheme.secondary).accessibilityIdentifier("simulator.live.steps")
            row {
                field("步数", $stepsText, label: "追加步行步数")
                Text("步（1…100000）").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Spacer(minLength: 4)
                apply("追加步行", parse(stepsText, SimulatorLiveState.addStepsRange)) { ["addSteps": .number(Double($0))] }
                    .accessibilityIdentifier("simulator.live.walk")
            }
            row {
                field("步频", $cadenceText, label: "步频，每分钟步数")
                Text("步/分（40…200）").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Spacer(minLength: 4)
                apply("应用步频", parse(cadenceText, InstanceFirmwareSettings.cadenceRange)) { ["cadence": .number(Double($0))] }
            }
        } else { unsupported("步数", live, key: "steps") }
        if live.supports("heartRate") {
            row {
                field("心率", $heartRateText, label: "心率，每分钟次数")
                Text("次/分（0 = 未佩戴）").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Spacer(minLength: 4)
                apply("应用心率", parse(heartRateText, 0...InstanceFirmwareSettings.maximumHeartRate)) { ["heartRate": .number(Double($0))] }
                    .accessibilityIdentifier("simulator.live.heart-rate")
            }
        } else { unsupported("心率", live, key: "heartRate") }
        if live.supports("steps") || live.supports("heartRate") {
            note("步数和心率由固件自己的算法从模拟的传感器数据中计算：步行开始的几步用于识别，心率在固件测量时读到设定脉搏，可能相差 1–3 次/分。")
        }
        Divider()
        // Board: charger, battery, temperature, clock.
        if live.supports("charger") {
            row {
                Picker("充电状态", selection: $charger) {
                    ForEach(SimulatorLiveCharger.allCases, id: \.self) { Text($0.title).tag($0) }
                }.frame(maxWidth: 260)
                Spacer(minLength: 4)
                button("应用", enabled: true) { ["charger": .string(charger.rawValue)] }
                    .accessibilityIdentifier("simulator.live.charger")
            }
        } else { unsupported("充电状态", live, key: "charger") }
        if live.supports("batteryLevel") {
            row {
                field("电量", $levelText, label: "电池电量百分比")
                Text("%（0…100）").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Spacer(minLength: 4)
                apply("应用电量", parse(levelText, SimulatorLiveState.batteryLevelRange)) { ["batteryLevel": .number(Double($0))] }
                    .accessibilityIdentifier("simulator.live.battery-level")
            }
            note("把电池电压改为该电量的开路电压；固件电量计按自己的算法逐步追随，不会立刻跳变，实际电量以设备屏幕为准。")
        } else { unsupported("电量", live, key: "batteryLevel") }
        if live.supports("batteryRaw") {
            DisclosureGroup("高级：电池电压采样原始值") {
                row {
                    field("原始值", $rawText, label: "电池电压采样原始值")
                    Text("0…65535").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                    Spacer(minLength: 4)
                    apply("应用原始值", parse(rawText, 0...InstanceFirmwareSettings.maximumSampleRaw)) { ["batteryRaw": .number(Double($0))] }
                }
            }
        }
        if live.supports("temperature") {
            row {
                field("温度", $temperatureText, label: "电池温度，摄氏度")
                Text("°C").font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
                Spacer(minLength: 4)
                button("应用温度", enabled: temperature != nil) { ["temperatureC": .number(temperature ?? 0)] }
            }
        } else { unsupported("温度", live, key: "temperature") }
        if live.supports("clock") {
            row {
                DatePicker("设备时间", selection: $clockDate, displayedComponents: [.date, .hourAndMinute])
                Spacer(minLength: 4)
                button("应用时间", enabled: true) { ["clock": .number(clockDate.timeIntervalSince1970.rounded())] }
            }
        } else { unsupported("设备时间", live, key: "clock") }
    }

    // MARK: - Pieces

    private func row<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        HStack(spacing: 8) { content() }
    }

    private func field(_ title: String, _ text: Binding<String>, label: String) -> some View {
        TextField(title, text: text).textFieldStyle(.roundedBorder).frame(width: 90).accessibilityLabel(label)
    }

    private func apply(_ title: String, _ value: Int?, _ make: @escaping (Int) -> [String: SimulatorJSON]) -> some View {
        button(title, enabled: value != nil) { make(value ?? 0) }
    }

    private func button(_ title: String, enabled: Bool, _ make: @escaping () -> [String: SimulatorJSON]) -> some View {
        Button(title) { send(make()) }.disabled(!self.enabled || busy || !enabled)
    }

    private func note(_ text: String) -> some View {
        Text(text).font(.system(size: 12)).foregroundStyle(SimulatorTheme.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder private func unsupported(_ title: String, _ live: SimulatorLiveState, key: String) -> some View {
        note("\(title)：此型号或当前核心不支持实时修改" + (live.notes[key].map { "（\($0)）" } ?? "") + "。")
    }

    private func stepsSummary(_ live: SimulatorLiveState) -> String {
        var parts = ["已走 \(live.int("walked") ?? 0) 步"]
        if live.stepsRemaining > 0 { parts.append("还要走 \(live.stepsRemaining) 步") }
        if let cadence = live.int("cadence") { parts.append("步频 \(cadence) 步/分") }
        if let rate = live.int("heartRate") { parts.append(rate == 0 ? "未佩戴" : "心率 \(rate) 次/分") }
        return parts.joined(separator: " · ")
    }

    private func parse(_ text: String, _ range: ClosedRange<Int>) -> Int? {
        Int(text.trimmingCharacters(in: .whitespaces)).flatMap { range.contains($0) ? $0 : nil }
    }

    private var temperature: Double? {
        Double(temperatureText.trimmingCharacters(in: .whitespaces)).flatMap { $0.isFinite && (-40...85).contains($0) ? $0 : nil }
    }

    // MARK: - Session

    /// One change per request, as the protocol requires.
    private func send(_ value: [String: SimulatorJSON]) {
        Task {
            busy = true; defer { busy = false }
            do {
                receive(try await session.liveCommand(SimulatorLiveCommands.set, arguments: value), fillDrafts: true)
                message = nil; outcomeUnknown = false
            } catch {
                let text = error.localizedDescription
                outcomeUnknown = text.contains("[live_outcome_unknown]")
                message = outcomeUnknown ? "写入可能已生效但未得到确认，请看设备画面或等待刷新，不要重复提交。" : text
            }
        }
    }

    private func receive(_ reply: SimulatorJSON, fillDrafts: Bool) {
        guard let next = SimulatorLiveState(reply) else { return }
        let first = live == nil
        live = next; unavailable = nil
        guard first || fillDrafts else { return }
        if let value = next.int("cadence") { cadenceText = String(value) }
        if let value = next.int("heartRate") { heartRateText = String(value) }
        if let value = next.charger { charger = value }
        if let value = next.int("batteryLevel") { levelText = String(value) }
        if let value = next.int("batteryRaw") { rawText = String(value) }
        if let value = next.number("temperatureC") { temperatureText = String(format: "%.1f", value) }
        if let value = next.number("clock") { clockDate = Date(timeIntervalSince1970: value) }
    }

    /// Refreshes while the device runs: faster while steps are still being walked.
    private func poll() async {
        live = nil; unavailable = nil; message = nil; outcomeUnknown = false
        guard session.isRunning else { return }
        while !Task.isCancelled, session.isRunning {
            if !busy {
                do { receive(try await session.liveCommand(SimulatorLiveCommands.get), fillDrafts: false) }
                catch {
                    if live == nil {
                        let text = error.localizedDescription
                        unavailable = text.contains("[unsupported") || text.contains("不受支持")
                            ? "当前运行的核心不提供实时输入。" : "暂时读不到实时输入：\(text)"
                    }
                }
            }
            let walking = (live?.stepsRemaining ?? 0) > 0
            do { try await Task.sleep(for: .seconds(walking ? 1 : 3)) } catch { break }
        }
    }
}
