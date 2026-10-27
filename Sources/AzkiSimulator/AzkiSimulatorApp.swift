import AppKit
import SwiftUI

@main @MainActor
struct AzkiSimulatorApp: App {
    @NSApplicationDelegateAdaptor(SimulatorAppDelegate.self) private var delegate
    @State private var library = SimulatorInstanceLibrary(maximumRuntimeSeconds: StandaloneRunLimits.unlimited)

    var body: some Scene {
        Window("Azki Simulator", id: "main") {
            SimulatorInstanceManagerView(library: library)
                .onAppear { delegate.library = library }
        }
        .defaultSize(width: 1_100, height: 760)
        .windowResizability(.contentMinSize)
        .windowToolbarStyle(.unified)
        .commands { SimulatorCommands() }

        Window("存储管理", id: "storage") {
            SimulatorStorageWindow(library: library)
        }
        .defaultSize(width: 760, height: 620)

        WindowGroup(id: "device-display", for: UUID.self) { $instanceID in
            if let instanceID {
                SimulatorAuxiliaryDeviceWindow(library: library, instanceID: instanceID)
            }
        }
        .defaultSize(width: 420, height: 780)

        Settings {
            SimulatorSettingsView(library: library)
        }
    }
}

@MainActor final class SimulatorAppDelegate: NSObject, NSApplicationDelegate {
    var library: SimulatorInstanceLibrary?
    private var terminating = false
    private var signalled = false
    private var termination: DispatchSourceSignal?

    func applicationDidFinishLaunching(_ notification: Notification) {
        signal(SIGTERM, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        source.setEventHandler { [weak self] in
            self?.signalled = true
            RunLoop.main.perform { NSApp.terminate(nil) }
        }
        source.resume()
        termination = source
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let library else { return .terminateNow }
        guard !terminating else { return .terminateLater }
        terminating = true
        Task {
            let failed = await library.stopAll()
            guard !failed.isEmpty, !signalled else {
                sender.reply(toApplicationShouldTerminate: true)
                return
            }
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "部分设备未能确认停止"
            alert.informativeText = "「\(failed.joined(separator: "」「"))」的设备进程尚未确认退出。仍然退出可能留下后台进程，下次启动时会自动清理临时会话。"
            alert.addButton(withTitle: "仍然退出")
            alert.addButton(withTitle: "取消")
            alert.buttons[0].keyEquivalent = ""
            alert.buttons[1].keyEquivalent = "\r"
            let quit = alert.runModal() == .alertFirstButtonReturn
            terminating = false
            sender.reply(toApplicationShouldTerminate: quit)
        }
        return .terminateLater
    }
}
