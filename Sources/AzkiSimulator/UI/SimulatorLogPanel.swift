import AppKit
import SwiftUI

@MainActor struct SimulatorLogPanel: View {
    let runtime: StandaloneRuntimeModel
    @Bindable var stream: SimulatorLogStreamModel
    let instanceName: String?
    var allowsReconnect = true

    @Environment(\.colorScheme) private var colorScheme
    private var session: SimulatorSessionModel { runtime.runtimeSession }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Image(systemName: "text.alignleft")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(GeminiPalette.accent(for: colorScheme))
                    Text(instanceName.map { "\($0) · 实时日志" } ?? "实时日志")
                        .font(.system(size: 12.5, weight: .semibold))
                        .accessibilityAddTraits(.isHeader)
                }
                Spacer(minLength: 8)
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 8) { controls }
                    VStack(alignment: .trailing, spacing: 6) { controls }
                }
            }

            if session.isAttached && stream.detail != "实时刷新 · 最多保留 256 KiB" {
                Text(stream.detail).font(.caption).foregroundStyle(.secondary)
            }

            SimulatorLogTextView(text: stream.text, followBottom: stream.followBottom,
                                 discardedUTF16: stream.discardedUTF16, epoch: stream.epoch)
                .padding(8)
                .frame(maxWidth: .infinity, minHeight: 70, maxHeight: .infinity,
                       alignment: .leading)
                .background(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(GeminiPalette.cardFill(for: colorScheme, elevated: false))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(GeminiPalette.hairlineBorder(for: colorScheme), lineWidth: 1)
                )
                .accessibilityIdentifier("simulator.live-log")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(minWidth: 0, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(GeminiPalette.stageBackground(for: colorScheme).opacity(0.92))
    }

    @ViewBuilder private var controls: some View {
        // Always selectable so users can pre-pick UART / QEMU / BLE logs even before booting.
        Picker("日志来源", selection: $stream.selectedName) {
            Text("UART 0").tag("uart0.log")
            Text("UART 1").tag("uart1.log")
            Text("UART 2").tag("uart2.log")
            Text("QEMU").tag("qemu.log")
            Text("虚拟手环连接").tag("virtual-peer.log")
            Text("标准错误").tag("stderr.log")
        }
        .labelsHidden()
        .controlSize(.small)
        .frame(width: 130)
        .accessibilityIdentifier("simulator.log-source")

        Toggle("跟随最新", isOn: $stream.followBottom)
            .toggleStyle(.checkbox)
            .controlSize(.small)
            .accessibilityIdentifier("simulator.log-follow")

        Button {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(stream.text, forType: .string)
        } label: {
            Label("拷贝", systemImage: "doc.on.doc")
        }
        .buttonStyle(GeminiNeonButtonStyle(tone: .subtle, compact: true))
        .disabled(stream.text.isEmpty)

        Button {
            stream.clear()
        } label: {
            Label("清空", systemImage: "trash")
        }
        .buttonStyle(GeminiNeonButtonStyle(tone: .subtle, compact: true))
        .disabled(stream.text.isEmpty)

        Button {
            if let id = session.manifest?.id, let directory = runtime.sessionURL {
                stream.start(directory: directory, sessionID: id, name: stream.selectedName)
            }
        } label: {
            Label("重连", systemImage: "arrow.clockwise")
        }
        .buttonStyle(GeminiNeonButtonStyle(tone: .secondary, compact: true))
        .disabled(!allowsReconnect || !session.isAttached)
        .accessibilityIdentifier("simulator.log-reconnect")
    }
}
