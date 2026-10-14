import Foundation
import Darwin

/// Operates only on a Process created and retained by this application. A group
/// is supplied only after the native broker observes PID == PGID before GO.
nonisolated enum StandaloneProcessReaper {
    static func stop(_ process: Process, confirmedGroup group: pid_t? = nil,
                     graceSeconds: Double = 3, killSeconds: Double = 2) async -> Bool {
        await Task.detached {
            // Foundation's terminate() may signal the process group. Give the
            // retained supervisor alone the chance to flush/stop its children;
            // the confirmed group is reserved for bounded forced cleanup below.
            if process.isRunning, process.processIdentifier > 1 {
                _ = Darwin.kill(process.processIdentifier, SIGTERM)
            }
            let grace = ContinuousClock.now + .seconds(graceSeconds)
            while (process.isRunning || groupExists(group)), ContinuousClock.now < grace {
                try? await Task.sleep(for: .milliseconds(25))
            }
            if let group, groupExists(group) { _ = Darwin.kill(-group, SIGKILL) }
            if process.isRunning { _ = Darwin.kill(process.processIdentifier, SIGKILL) }
            let deadline = ContinuousClock.now + .seconds(killSeconds)
            while (process.isRunning || groupExists(group)), ContinuousClock.now < deadline {
                try? await Task.sleep(for: .milliseconds(25))
            }
            return !process.isRunning && !groupExists(group)
        }.value
    }
    static func groupExists(_ group: pid_t?) -> Bool {
        guard let group, group > 1 else { return false }
        return Darwin.kill(-group, 0) == 0 || errno != ESRCH
    }
}
