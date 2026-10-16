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

/// Waits for a main-actor operation at most `seconds`. The operation itself
/// keeps running; a timeout only means "not confirmed", so one stuck instance
/// cannot hold app-wide work (quit, launch cleanup) forever.
@MainActor enum SimulatorDeadline {
    private final class Once { var done = false }
    static func bounded<T: Sendable>(_ seconds: Double, fallback: T,
                                     _ operation: @escaping @MainActor () async -> T) async -> T {
        await withCheckedContinuation { (continuation: CheckedContinuation<T, Never>) in
            let once = Once()
            Task { @MainActor in
                let value = await operation()
                guard !once.done else { return }
                once.done = true
                continuation.resume(returning: value)
            }
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(seconds))
                guard !once.done else { return }
                once.done = true
                continuation.resume(returning: fallback)
            }
        }
    }
}
