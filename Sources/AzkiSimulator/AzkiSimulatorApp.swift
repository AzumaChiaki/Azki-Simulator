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
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

/// Quitting waits for the app-owned helper/core to stop. An attached external
/// session only loses this UI's connection; it is not implicitly terminated.
@MainActor final class SimulatorAppDelegate: NSObject, NSApplicationDelegate {
    var library: SimulatorInstanceLibrary?
    private var terminating = false
    private var termination: DispatchSourceSignal?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // SIGTERM (logout, kill, launchd) takes the same path as Quit, so the
        // app-owned core and helper are stopped instead of being orphaned.
        signal(SIGTERM, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        // Hop to a run-loop block: terminating inside this main-queue callout would
        // block the main queue that the asynchronous stop (terminateLater) needs.
        source.setEventHandler { RunLoop.main.perform { NSApp.terminate(nil) } }
        source.resume()
        termination = source
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let library else { return .terminateNow }
        guard !terminating else { return .terminateLater }
        terminating = true
        Task {
            if !(await library.stopAll()) {
                terminating = false
                sender.reply(toApplicationShouldTerminate: false)
                return
            }
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }
}
