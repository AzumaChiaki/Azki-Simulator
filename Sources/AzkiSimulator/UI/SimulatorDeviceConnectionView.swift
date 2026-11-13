import SwiftUI
import AppKit

@MainActor struct SimulatorDeviceConnectionView: View {
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext
    @ObservedObject var bluetooth: BluetoothExternalSession
    @State private var serialDevices: [SimulatorSerialDevice] = []
    @State private var confirmFlash = false
    @State private var restoreBackup: URL?
    @State private var confirmRestore = false

    var body: some View {
        Group {
            Toggle("跳过首次设置（OOBE）", isOn: Binding(
                get: { instance.record.skipOOBE ?? true },
                set: { value in
                    do { try library.setSkipOOBE(instance.id, enabled: value) }
                    catch { library.errorMessage = error.localizedDescription }
                }))
                .disabled(instance.runtime.isBusy || instance.runtime.isServing || instance.runtime.runtimeSession.isAttached)
                .accessibilityIdentifier("simulator.skip-oobe")
            Text("仅影响新实例的首次设置；已有设备保留应用、设置和配对记录。")
            bluetoothSection
            debuggerSection
        }
    }

    private var connectionBusy: Bool {
        instance.runtime.isBusy || instance.runtime.isServing || instance.runtime.runtimeSession.isAttached
    }

    private func saveBluetooth(mode: SimulatorBluetoothMode, port: String) {
        Task {
            do { try await library.setBluetooth(instance.id, mode: mode, port: port) }
            catch { library.errorMessage = error.localizedDescription }
        }
    }

    @ViewBuilder private var bluetoothSection: some View {
        Picker("蓝牙连接", selection: Binding(
            get: { instance.record.bluetoothMode ?? .virtual },
            set: { saveBluetooth(mode: $0, port: instance.record.esp32Port ?? "") })) {
            ForEach(SimulatorBluetoothMode.allCases, id: \.self) { mode in Text(mode.label).tag(mode) }
        }
        .disabled(connectionBusy)
        .accessibilityIdentifier("simulator.bluetooth.mode")
        if (instance.record.bluetoothMode ?? .virtual) == .virtual {
            LabeledContent("外部蓝牙") {
                if bluetooth.isActive || bluetooth.isStarting {
                    Button("关闭广播") { bluetooth.stop() }
                        .accessibilityIdentifier("simulator.bluetooth.stop")
                } else {
                    Button("开启广播") {
                        bluetooth.start(localName: instance.record.name,
                            requestSeconds: Double(instance.runtime.connectionSettings.timeoutSeconds))
                    }
                    .disabled(!instance.runtime.runtimeSession.isRunning || instance.runtime.isStopping)
                    .accessibilityIdentifier("simulator.bluetooth.start")
                }
            }
            Text(bluetooth.statusText).accessibilityIdentifier("simulator.bluetooth.status")
            Text("Mac 同时对外广播一台虚拟设备，开启另一台会自动切换。")
            if let error = bluetooth.lastError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
        } else {
            Text("当前支持经典 ESP32（4 MB 闪存）桥接 Q66 固件。")
            Picker("ESP32 设备", selection: Binding(
                get: { instance.record.esp32Port ?? "" },
                set: { saveBluetooth(mode: .esp32, port: $0) })) {
                Text("请选择 USB 串口").tag("")
                ForEach(serialDevices) { device in Text(device.label).tag(device.path) }
                if let selected = instance.record.esp32Port, !selected.isEmpty, !serialDevices.contains(where: { $0.path == selected }) {
                    Text("未连接：" + selected).tag(selected)
                }
            }
            .disabled(connectionBusy)
            .accessibilityIdentifier("simulator.bluetooth.esp32-port")
            HStack {
                Button("刷新设备") { serialDevices = SimulatorSerialDevice.connected() }.disabled(connectionBusy)
                Button("刷入桥接固件…") { confirmFlash = true }
                    .disabled(connectionBusy || (instance.record.esp32Port ?? "").isEmpty)
                Button("恢复备份…") {
                    let panel = NSOpenPanel()
                    panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
                    panel.message = "选择此 ESP32 的完整固件备份"
                    if panel.runModal() == .OK, let url = panel.url { restoreBackup = url; confirmRestore = true }
                }.disabled(connectionBusy || (instance.record.esp32Port ?? "").isEmpty)
            }
            Button("打开备份目录") {
                let folder = BundledFirmwareService.libraryRoot.appendingPathComponent("esp32-backups")
                NSWorkspace.shared.open(folder)
            }
            Text(instance.runtime.isServing ? "ESP32 桥接随此设备运行，停止设备时释放串口。" : "选好设备后点击「运行」。首次运行会将此实例绑定到 ESP32 模式。")
            Text("已有 Mac 广播实例请新建实例后选择 ESP32，以保留原设备配置和配对记录。")
            Button("新建设备实例") { library.creationRequested = true }.disabled(connectionBusy)
            if let message = instance.runtime.esp32FlashMessage { Text(message).textSelection(.enabled) }
        }
        Text("连接方式与串口选择在下次启动时生效。")
            .task { serialDevices = SimulatorSerialDevice.connected() }
            .confirmationDialog("刷入 ESP32 桥接固件？", isPresented: $confirmFlash, titleVisibility: .visible) {
                Button("备份并刷入", role: .destructive) { Task { await instance.runtime.flashESP32() } }
            } message: {
                Text("将覆盖 \(instance.record.esp32Port ?? "") 上的固件。应用会先保存完整备份，备份成功后才刷写。完整备份可能需要数分钟，请确认这是要使用的 ESP32，并保持连接。")
            }
            .confirmationDialog("恢复 ESP32 固件备份？", isPresented: $confirmRestore, titleVisibility: .visible) {
                Button("恢复备份", role: .destructive) {
                    if let backup = restoreBackup { Task { await instance.runtime.flashESP32(restore: backup) } }
                }
            } message: {
                Text("将用所选备份覆盖 \(instance.record.esp32Port ?? "") 当前的固件。请确认备份属于此设备，并保持连接。")
            }
    }

    @ViewBuilder private var debuggerSection: some View {
        let runtime = instance.runtime
        Toggle("启用 GDB 调试", isOn: Binding(get: { runtime.debuggerEnabled }, set: { runtime.debuggerEnabled = $0 }))
            .accessibilityIdentifier("simulator.gdb.enabled")
        if runtime.debuggerEnabled {
            Toggle("开机时暂停等待调试器", isOn: Binding(get: { runtime.debuggerWaitForAttach },
                                                set: { runtime.debuggerWaitForAttach = $0 }))
                .accessibilityIdentifier("simulator.gdb.wait")
            TextField("端口（留空自动分配）", value: Binding<Int?>(
                get: { runtime.debuggerPort == 0 ? nil : runtime.debuggerPort },
                set: { runtime.debuggerPort = min(max($0 ?? 0, 0), 65_535) }),
                format: .number.grouping(.never))
                .accessibilityIdentifier("simulator.gdb.port")
        }
        if let endpoint = runtime.debuggerEndpoint {
            let command = "target remote 127.0.0.1:\(endpoint.port)"
            LabeledContent("GDB") {
                HStack {
                    Text(command).font(.body.monospaced()).textSelection(.enabled)
                        .accessibilityIdentifier("simulator.gdb.endpoint")
                    Button("拷贝") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(command, forType: .string)
                    }
                }
            }
            if endpoint.waitForAttach {
                Text("CPU 停在复位处，调试器执行 continue 后开机。")
            }
        } else if runtime.isServing && runtime.debuggerEnabled {
            Text("本次运行未启用 GDB 或端口尚未就绪；更改在下次启动时生效。")
        }
        Text("仅监听本机 127.0.0.1；断点暂停期间蓝牙会话的超时计时随之暂停。")
    }
}
