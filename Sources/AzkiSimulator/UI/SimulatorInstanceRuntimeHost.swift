import SwiftUI

/// Keeps per-instance guest polling and log reading alive regardless of which window is visible.
@MainActor struct SimulatorInstanceRuntimeHost: View {
    let library: SimulatorInstanceLibrary
    let instance: SimulatorInstanceContext

    private var session: SimulatorSessionModel { instance.runtime.runtimeSession }
    private var guestAvailable: Bool {
        session.isRunning && session.status?["guestConnectSupported"] != .bool(false)
    }
    private var guestContext: String { "\(session.manifest?.id.uuidString ?? "none")/\(guestAvailable)" }

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .accessibilityHidden(true)
            .task {
                if !instance.runtime.hasLoadedCatalog { await instance.runtime.loadCatalog() }
            }
            .task(id: "\(guestContext)/\(instance.runtime.resetEpoch)") {
                instance.guestOperations.configure(
                    hostSessionID: session.manifest?.id, available: guestAvailable,
                    packagesAvailable: session.status?["guestInstallSupported"] != .bool(false),
                    command: { try await session.guestCommand($0, arguments: $1) },
                    install: { try await session.installGuestPackage($0, expectedSessionID: $1, expectedGeneration: $2) })
            }
            .task(id: "\(session.manifest?.id.uuidString ?? "none")/\(session.isAttached)/\(instance.logStream.selectedName)") {
                if session.isAttached, let id = session.manifest?.id, let directory = instance.runtime.sessionURL {
                    instance.logStream.startIfNeeded(directory: directory, sessionID: id, name: instance.logStream.selectedName)
                } else if !session.isAttached {
                    instance.logStream.stop()
                }
            }
            .onChange(of: instance.runtime.selectedProfileID) { _, _ in library.persistSelection(instance) }
            .onChange(of: session.isRunning) { _, running in
                if !running { instance.bluetooth.stop() }
            }
            .onChange(of: instance.runtime.isStopping) { _, stopping in
                if stopping { instance.bluetooth.stop() }
            }
            .onChange(of: instance.runtime.resetEpoch) { _, _ in
                instance.runReview = nil
                instance.logStream.clear()
                instance.guestOperations.invalidate()
            }
    }
}
