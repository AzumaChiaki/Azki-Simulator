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
        .defaultSize(width: 1_300, height: 860)
        .windowResizability(.contentMinSize)
        Settings {
            SimulatorSettingsView(library: library)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("新建设备实例…") { library.creationRequested = true }
                    .keyboardShortcut("n")
                    .disabled(!library.storageAvailable || library.deviceModels.isEmpty)
            }
        }
    }
}

/// Quitting waits for the app-owned helper/core to stop. An attached external
/// session only loses this UI's connection; it is not implicitly terminated.
@MainActor final class SimulatorAppDelegate: NSObject, NSApplicationDelegate {
    var library: SimulatorInstanceLibrary?
    private var terminating = false
    private var signalled = false
    private var termination: DispatchSourceSignal?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // SIGTERM (logout, kill, launchd) takes the same path as Quit, so the
        // app-owned core and helper are stopped instead of being orphaned.
        signal(SIGTERM, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        // Hop to a run-loop block: terminating inside this main-queue callout would
        // block the main queue that the asynchronous stop (terminateLater) needs.
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
            // Each instance's stop is bounded, so Quit always gets an answer.
            let failed = await library.stopAll()
            // Logout/kill cannot wait for a dialog; every stop was already tried.
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
            let quit = alert.runModal() == .alertFirstButtonReturn
            terminating = false
            sender.reply(toApplicationShouldTerminate: quit)
        }
        return .terminateLater
    }
}
