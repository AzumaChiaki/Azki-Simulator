import SwiftUI

@MainActor struct SimulatorLogPanel: View {
    let runtime: StandaloneRuntimeModel
    @Bindable var stream: SimulatorLogStreamModel
    let instanceName: String?
    var allowsReconnect = true

    private var session: SimulatorSessionModel { runtime.runtimeSession }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Picker("日志来源", selection: $stream.selectedName) {
                    Text("UART 0").tag("uart0.log")
                    Text("UART 1").tag("uart1.log")
                    Text("UART 2").tag("uart2.log")
                    Text("QEMU").tag("qemu.log")
                    Text("虚拟手环连接").tag("virtual-peer.log")
                    Text("标准错误").tag("stderr.log")
                }.disabled(!session.isAttached)
                    .accessibilityIdentifier("simulator.log-source")
                Toggle("跟随最新日志", isOn: $stream.followBottom).toggleStyle(.checkbox)
                    .accessibilityIdentifier("simulator.log-follow")
                Button("重连日志", systemImage: "arrow.clockwise") {
                    if let id = session.manifest?.id, let directory = runtime.sessionURL {
                        stream.start(directory: directory, sessionID: id, name: stream.selectedName)
                    }
                }
                .disabled(!allowsReconnect || !session.isAttached)
                .accessibilityIdentifier("simulator.log-reconnect")
            }
            if session.isAttached && stream.detail != "实时刷新 · 最多保留 256 KiB" {
                Text(stream.detail).font(.caption).foregroundStyle(.secondary)
            }
            SimulatorLogTextView(text: stream.text, followBottom: stream.followBottom,
                                 discardedUTF16: stream.discardedUTF16, epoch: stream.epoch)
                .frame(minHeight: 70, maxHeight: .infinity)
                .accessibilityIdentifier("simulator.live-log")
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}
