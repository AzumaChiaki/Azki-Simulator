import Foundation

/// Reusable, explicitly opened, in-memory fixture transport. It does not
/// construct a driver, execute QEMU, access files, or speak to a radio/network.
/// Successful operations mean fixture RAM changed, never guest installation,
/// pairing, authentication, or physical Bluetooth success.
@MainActor final class BluetoothOfflineGuestTransport: BluetoothGuestTransport {
    enum Failure: Error, Equatable {
        case invalidSession, unknownAttribute, permissionDenied, invalidOffset
        case invalidLength, notSubscribed, receiverMissing, exhausted
    }

    let fixtureOnly = true
    static let fixtureIdentityFingerprint = "azki.offline.bluetooth.fixture.v1"
    var identityFingerprint: String { Self.fixtureIdentityFingerprint }
    let maximumATTMTU: Int
    let services: [BluetoothServiceDefinition]
    var notificationHandler: ((BluetoothGuestNotification) -> Void)?
    private(set) var activeSessionID: UUID?
    private let definitions: [String: BluetoothCharacteristicDefinition]
    private let seeds: [String: Data]
    private let maximumOperations: Int
    private var values: [String: Data] = [:]
    private var subscriptions = Set<String>()
    private var retired = Set<UUID>()
    private var operations = 0

    init(services: [BluetoothServiceDefinition], initialValues: [String: Data] = [:],
         maximumATTMTU: Int = 23, maximumOperations: Int = 4096) throws {
        try BluetoothPeripheralConfiguration(localName: "Azki BLE Fixture", guestIdentity: Self.fixtureIdentityFingerprint,
                                             services: services).validate()
        guard (23...517).contains(maximumATTMTU), (1...4096).contains(maximumOperations) else {
            throw BluetoothAdapterError.invalidConfiguration("offline fixture limits")
        }
        let table = Dictionary(uniqueKeysWithValues: services.flatMap(\.characteristics).map { ($0.key, $0) })
        guard initialValues.allSatisfy({ key, value in
            guard let definition = table[key] else { return false }
            return value.count <= definition.maximumLength
        }) else { throw BluetoothAdapterError.invalidConfiguration("offline fixture seed") }
        self.services = services; self.maximumATTMTU = maximumATTMTU
        self.maximumOperations = maximumOperations; definitions = table
        seeds = table.mapValues { initialValues[$0.key] ?? Data() }
    }

    /// The host opens the adapter-generated session explicitly before guest I/O.
    /// At most 64 sessions per transport object; old session IDs cannot revive.
    /// Each session starts from the immutable fixture seeds, with no persistence.
    func openSession(_ session: UUID) throws {
        guard activeSessionID == nil, !retired.contains(session), retired.count < 64 else {
            throw Failure.invalidSession
        }
        activeSessionID = session; values = seeds; subscriptions.removeAll(); operations = 0
    }

    private func admit(_ session: UUID, key: String) throws -> BluetoothCharacteristicDefinition {
        guard session == activeSessionID else { throw Failure.invalidSession }
        guard let definition = definitions[key] else { throw Failure.unknownAttribute }
        guard operations < maximumOperations else { close(session: session); throw Failure.exhausted }
        operations += 1
        return definition
    }

    func read(session: UUID, key: String, offset: Int, maximumLength: Int) async throws -> Data {
        try Task.checkCancellation()
        let definition = try admit(session, key: key)
        guard definition.readable else { throw Failure.permissionDenied }
        let value = values[key] ?? Data()
        guard (0...value.count).contains(offset) else { throw Failure.invalidOffset }
        guard (0...512).contains(maximumLength) else { throw Failure.invalidLength }
        return Data(value.dropFirst(offset).prefix(min(maximumLength, maximumATTMTU - 1)))
    }

    func write(session: UUID, key: String, value: Data) async throws {
        try Task.checkCancellation()
        let definition = try admit(session, key: key)
        guard definition.writable else { throw Failure.permissionDenied }
        guard value.count <= min(definition.maximumLength, maximumATTMTU - 3) else { throw Failure.invalidLength }
        values[key] = value
    }

    func setNotifications(session: UUID, key: String, enabled: Bool) async throws {
        try Task.checkCancellation()
        let definition = try admit(session, key: key)
        guard definition.notifiable else { throw Failure.permissionDenied }
        if enabled { subscriptions.insert(key) } else { subscriptions.remove(key) }
    }

    /// Explicit fixture injection, never an external callback or real guest ACK.
    func injectNotification(session: UUID, key: String, value: Data) throws {
        let definition = try admit(session, key: key)
        guard definition.notifiable, subscriptions.contains(key) else { throw Failure.notSubscribed }
        guard value.count <= min(definition.maximumLength, maximumATTMTU - 3) else { throw Failure.invalidLength }
        guard let notificationHandler else { throw Failure.receiverMissing }
        values[key] = value
        notificationHandler(BluetoothGuestNotification(session: session, key: key, value: value))
    }

    func close(session: UUID) {
        guard activeSessionID == session else { return }
        retired.insert(session)
        activeSessionID = nil; values.removeAll(); subscriptions.removeAll(); operations = 0
        // The adapter owns the event handler lifetime and detaches it on stop.
    }
}
