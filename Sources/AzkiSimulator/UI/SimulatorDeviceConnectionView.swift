import SwiftUI

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
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.25))
    }
}
