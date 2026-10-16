import SwiftUI
import AppKit

@MainActor struct SimulatorDeviceConnectionView: View {
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext
    @ObservedObject var bluetooth: BluetoothExternalSession

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("跳过首次设置（OOBE）", isOn: Binding(
                get: { instance.record.skipOOBE ?? true },
                set: { value in
                    do { try library.setSkipOOBE(instance.id, enabled: value) }
                    catch { library.errorMessage = error.localizedDescription }
                }))
                .disabled(instance.runtime.isBusy || instance.runtime.isServing || instance.runtime.runtimeSession.isAttached)
                .accessibilityIdentifier("simulator.skip-oobe")
            Text("仅影响新实例的首次设置；已有设备保留应用、设置和配对记录。")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Label("外部蓝牙", systemImage: "antenna.radiowaves.left.and.right")
                Spacer()
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
            Text(bluetooth.statusText).font(.caption).foregroundStyle(.secondary)
                .accessibilityIdentifier("simulator.bluetooth.status")
            Text("Mac 同时对外广播一台虚拟设备，开启另一台会自动切换。")
                .font(.caption).foregroundStyle(.secondary)
            if let error = bluetooth.lastError {
                Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
            Divider()
            debuggerSection
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.25))
    }

    /// GDB remote server on loopback. It coexists with the virtual Bluetooth
    /// peer: protocol timers pause while the debugger holds the CPU.
    @ViewBuilder private var debuggerSection: some View {
        let runtime = instance.runtime
        Toggle("启用 GDB 调试", isOn: Binding(get: { runtime.debuggerEnabled }, set: { runtime.debuggerEnabled = $0 }))
            .accessibilityIdentifier("simulator.gdb.enabled")
        if runtime.debuggerEnabled {
            Toggle("开机时暂停等待调试器", isOn: Binding(get: { runtime.debuggerWaitForAttach },
                                                set: { runtime.debuggerWaitForAttach = $0 }))
                .accessibilityIdentifier("simulator.gdb.wait")
            HStack(spacing: 6) {
                Text("端口")
                TextField("自动", value: Binding<Int?>(
                    get: { runtime.debuggerPort == 0 ? nil : runtime.debuggerPort },
                    set: { runtime.debuggerPort = min(max($0 ?? 0, 0), 65_535) }),
                    format: .number.grouping(.never))
                    .frame(width: 72)
                    .accessibilityIdentifier("simulator.gdb.port")
                Text("留空自动分配空闲端口").font(.caption).foregroundStyle(.secondary)
            }
        }
        if let endpoint = runtime.debuggerEndpoint {
            let command = "target remote 127.0.0.1:\(endpoint.port)"
            HStack(spacing: 6) {
                Text("GDB：" + command).font(.caption.monospaced()).textSelection(.enabled)
                    .accessibilityIdentifier("simulator.gdb.endpoint")
                Button("拷贝") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(command, forType: .string)
                }
                .controlSize(.small)
            }
            if endpoint.waitForAttach {
                Text("CPU 停在复位处，调试器执行 continue 后开机。").font(.caption).foregroundStyle(.secondary)
            }
        } else if runtime.isServing && runtime.debuggerEnabled {
            Text("本次运行未启用 GDB 或端口尚未就绪；更改在下次启动时生效。").font(.caption).foregroundStyle(.secondary)
        }
        Text("仅监听本机 127.0.0.1，可与虚拟蓝牙同时使用；断点暂停期间蓝牙会话的超时计时随之暂停。调试器异常断开时设备自动继续运行。")
            .font(.caption).foregroundStyle(.secondary)
    }
}
