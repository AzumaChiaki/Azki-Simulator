import Foundation

enum BluetoothAdapterError: Error, Equatable, LocalizedError, CustomStringConvertible {
    case invalidConfiguration(String)
    case unsupported(String)
    case unavailable(String)
    var errorDescription: String? { description }
    var description: String {
        switch self {
        case .invalidConfiguration(let reason), .unsupported(let reason), .unavailable(let reason): return reason
        }
    }
}

struct BluetoothCharacteristicDefinition: Equatable, Sendable {
    let key: String
    let uuid: String
    var readable = false
    var writable = false
    var notifiable = false
    var maximumLength = 512
    var requiresEncryption = false
    var indicates = false
    var writeWithoutResponse = false
    // Optional for older definitions whose writable/notifiable fields aggregate
    // both ATT modes. Discovered services supply the two independent bits.
    var writeWithResponse: Bool? = nil
    var notifies: Bool? = nil
}

struct BluetoothServiceDefinition: Equatable, Sendable {
    let key: String
    let uuid: String
    let characteristics: [BluetoothCharacteristicDefinition]
}

struct BluetoothPeripheralConfiguration: Sendable {
    let localName: String
    let guestIdentity: String
    let services: [BluetoothServiceDefinition]
    var requestSeconds: TimeInterval = 30

    // The full GATT database is independent of the small advertising packet.
    // Keep FE95 in the normal advertising UUID list instead of letting unrelated
    // services and a long instance name consume its available space.
    var advertisingServiceUUIDs: [String] {
        let candidates = services.map { Self.compactBluetoothUUID($0.uuid) }
        if candidates.contains("FE95") { return ["FE95"] }
        guard let smallest = candidates.min(by: {
            $0.count == $1.count ? $0 < $1 : $0.count < $1.count
        }) else { return [] }
        return [smallest]
    }

    var advertisingName: String {
        let trimmed = localName.trimmingCharacters(in: .whitespacesAndNewlines)
        var result = ""
        for character in trimmed {
            let next = String(character)
            guard result.utf8.count + next.utf8.count <= 10 else { break }
            result += next
        }
        return result.isEmpty ? "Azki" : result
    }

    private static func compactBluetoothUUID(_ uuid: String) -> String {
        var value = uuid.uppercased()
        let baseSuffix = "-0000-1000-8000-00805F9B34FB"
        if value.count == 36, value.hasSuffix(baseSuffix) {
            value = String(value.prefix(8))
        }
        if value.count == 8, value.hasPrefix("0000") {
            value = String(value.suffix(4))
        }
        return value
    }

    func validate() throws {
        guard localName.utf8.count <= 248,
              requestSeconds.isFinite, (0.05...300).contains(requestSeconds),
              (1...64).contains(services.count) else {
            throw BluetoothAdapterError.invalidConfiguration("name, request timeout or service count")
        }
        var keys = Set<String>()
        var count = 0
        for service in services {
            guard Self.validKey(service.key), Self.validUUID(service.uuid),
                  keys.insert(service.key).inserted, !service.characteristics.isEmpty else {
                throw BluetoothAdapterError.invalidConfiguration("service instance")
            }
            for item in service.characteristics {
                count += 1
                guard Self.validKey(item.key), keys.insert(item.key).inserted,
                      Self.validUUID(item.uuid), (1...512).contains(item.maximumLength),
                      item.readable || item.writable || item.notifiable else {
                    throw BluetoothAdapterError.invalidConfiguration("characteristic instance")
                }
            }
        }
        guard count <= 256 else { throw BluetoothAdapterError.invalidConfiguration("too many characteristics") }
    }

    private static func validKey(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 128 && value.utf8.allSatisfy {
            (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) || [45, 46, 58, 64, 95].contains($0)
        }
    }
    private static func validUUID(_ value: String) -> Bool {
        ((value.count == 4 || value.count == 8) && value.allSatisfy({ $0.isHexDigit })) || UUID(uuidString: value) != nil
    }
}

enum BluetoothPeripheralPower: Equatable { case unknown, resetting, unsupported, unauthorized, poweredOff, poweredOn }
enum BluetoothATTResult: Equatable { case success, invalidHandle, readNotPermitted, writeNotPermitted, invalidOffset, invalidLength, insufficientResources, unlikelyError, requestNotSupported; case guestError(Int) }
struct BluetoothGuestATTError: Error { let code: Int }
struct BluetoothReadRequest { let id: UUID; let peer: UUID; let key: String; let offset: Int }
struct BluetoothWriteValue { let key: String; let offset: Int; let value: Data }
struct BluetoothWriteRequest { let id: UUID; let peer: UUID; let values: [BluetoothWriteValue] }
struct BluetoothGuestNotification { let session: UUID; let key: String; let value: Data }

enum BluetoothDriverEvent {
    case power(BluetoothPeripheralPower)
    case servicesPublished(error: String?)
    case advertisingStarted(error: String?)
    case read(BluetoothReadRequest)
    case write(BluetoothWriteRequest)
    case subscribed(peer: UUID, key: String, maximumValueLength: Int)
    case unsubscribed(peer: UUID, key: String)
    case ready
}

@MainActor protocol BluetoothPeripheralDriver: AnyObject {
    var eventHandler: ((BluetoothDriverEvent) -> Void)? { get set }
    func start() throws
    func publish(_ services: [BluetoothServiceDefinition])
    func advertise(localName: String, serviceUUIDs: [String])
    func respond(to request: UUID, value: Data?, result: BluetoothATTResult)
    func update(_ value: Data, characteristic key: String, peer: UUID) -> Bool
    func stop()
}

/// A transport is session-scoped. It MUST reject work/notifications for a closed
/// session even if an async operation completes late. No concrete QEMU or HCI
/// backend is constructed by this protocol, and no authentication result is invented.
@MainActor protocol BluetoothGuestTransport: AnyObject {
    var identityFingerprint: String { get }
    var maximumATTMTU: Int { get }
    var notificationHandler: ((BluetoothGuestNotification) -> Void)? { get set }
    func read(session: UUID, key: String, offset: Int, maximumLength: Int) async throws -> Data
    func write(session: UUID, key: String, value: Data) async throws
    func setNotifications(session: UUID, key: String, enabled: Bool) async throws
    func close(session: UUID)
    func openAdapterSession(_ session: UUID)
    func reconnect(session: UUID) async throws
}

extension BluetoothGuestTransport {
    func openAdapterSession(_ session: UUID) {}
    func reconnect(session: UUID) async throws {}
}

/// Does not construct a driver or manager during init. Host code must call
/// startFromUserAction only from the user’s explicit external Bluetooth action.
@MainActor final class BluetoothPeripheralAdapter {
    enum State: Equatable { case stopped, waitingForPower, publishing, advertisingPending, advertising, failed(String) }
    private(set) var state: State = .stopped
    private(set) var lastError: String?
    private(set) var acceptedByHostQueue = 0 // NOT radio delivery or application ACK.
    private(set) var sessionID: UUID?
    var queuedNotificationCount: Int { notifications.count }
    var pendingRequestCount: Int { pending.count }

    private struct Pending { let deadline: TimeInterval; let mutatesGuest: Bool; let task: Task<Void, Never> }
    private struct Notification { let key: String; let value: Data; let peer: UUID }
    private let guest: BluetoothGuestTransport
    private let factory: () -> BluetoothPeripheralDriver
    private let now: () -> TimeInterval
    private let automaticPolling: Bool
    private var driver: BluetoothPeripheralDriver?
    private var configuration: BluetoothPeripheralConfiguration?
    private var attributes: [String: BluetoothCharacteristicDefinition] = [:]
    private var pending: [UUID: Pending] = [:]
    private var subscriptionTasks: [String: Task<Void, Never>] = [:]
    private var subscriptionTokens: [String: UUID] = [:]
    private var subscriptionDeadlines: [String: TimeInterval] = [:]
    private var subscriptions = Set<String>()
    private var guestSubscriptions = Set<String>()
    private var releasePeerWhenIdle = false
    private var reconnectTask: Task<Void, Error>?
    private var reconnectDeadline: TimeInterval?
    private var notifications: [Notification] = []
    private var notificationBytes = 0
    private var blocked = false
    private var peer: UUID?
    private var peerLimit = 20
    private var guestMTU = 23
    private var generation: UInt64 = 0
    private var polling: Task<Void, Never>?

    init(guest: BluetoothGuestTransport,
         driverFactory: (() -> BluetoothPeripheralDriver)? = nil,
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         automaticPolling: Bool = true) {
        self.guest = guest; factory = driverFactory ?? { CoreBluetoothPeripheralDriver() }
        self.now = now; self.automaticPolling = automaticPolling
    }

    func startFromUserAction(configuration: BluetoothPeripheralConfiguration) throws {
        guard driver == nil else { throw BluetoothAdapterError.unavailable("already started") }
        try configuration.validate()
        guard (23...517).contains(guest.maximumATTMTU), now().isFinite else {
            throw BluetoothAdapterError.invalidConfiguration("guest MTU")
        }
        generation &+= 1
        let token = generation
        let session = UUID()
        sessionID = session
        guestMTU = guest.maximumATTMTU
        self.configuration = configuration
        attributes = Dictionary(uniqueKeysWithValues: configuration.services.flatMap(\.characteristics).map { ($0.key, $0) })
        guest.openAdapterSession(session)
        lastError = nil; acceptedByHostQueue = 0; state = .waitingForPower
        let created = factory()
        driver = created
        created.eventHandler = { [weak self] event in
            guard let self, self.generation == token else { return }
            self.receive(event)
        }
        guest.notificationHandler = { [weak self] event in self?.receiveNotification(event) }
        do { try created.start() }
        catch { fail("Bluetooth driver could not start: \(error.localizedDescription)"); throw error }
        if automaticPolling, driver != nil {
            polling = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
                    guard self != nil else { return }
                    self?.pollDeadlines()
                }
            }
        }
    }

    /// Stops only this manager's advertising/services and this guest session.
    /// Cannot claim to disconnect all system BLE connections or other apps.
    func stop() {
        generation &+= 1
        polling?.cancel(); polling = nil
        reconnectTask?.cancel(); reconnectTask = nil; reconnectDeadline = nil
        for (id, item) in pending { item.task.cancel(); driver?.respond(to: id, value: nil, result: .unlikelyError) }
        pending.removeAll()
        subscriptionTasks.values.forEach { $0.cancel() }
        subscriptionTasks.removeAll(); subscriptionTokens.removeAll(); subscriptionDeadlines.removeAll()
        guest.notificationHandler = nil
        if let sessionID { guest.close(session: sessionID) }
        driver?.eventHandler = nil
        driver?.stop(); driver = nil
        sessionID = nil; configuration = nil; attributes.removeAll()
        subscriptions.removeAll(); guestSubscriptions.removeAll(); notifications.removeAll(); notificationBytes = 0
        releasePeerWhenIdle = false
        peer = nil; peerLimit = 20; blocked = false; state = .stopped
    }

    func pollDeadlines() {
        guard driver != nil else { return }
        let time = now()
        guard time.isFinite else { fail("Invalid monotonic time"); return }
        if let reconnectDeadline, reconnectDeadline <= time {
            fail("Guest reconnect timed out; session was closed")
            return
        }
        if subscriptionDeadlines.values.contains(where: { $0 <= time }) {
            fail("Guest subscription timed out; outcome is unknown and session was closed")
            return
        }
        if pending.values.contains(where: { $0.mutatesGuest && $0.deadline <= time }) {
            // Cancellation cannot undo an already submitted guest mutation.
            // Close the entire session before accepting any subsequent write.
            fail("Guest write timed out; outcome is unknown and session was closed")
            return
        }
        for id in pending.filter({ $0.value.deadline <= time }).map(\.key) {
            pending.removeValue(forKey: id)?.task.cancel()
            driver?.respond(to: id, value: nil, result: .unlikelyError)
        }
        releaseIdlePeer()
    }

    private func fail(_ error: String) { stop(); lastError = error; state = .failed(error) }
    private func validPeer(_ candidate: UUID) -> Bool {
        if peer == nil { peer = candidate }
        return peer == candidate
    }
    private func releaseIdlePeer() {
        // Unsubscription is not proof of physical disconnection. It ends this
        // peer's logical notification session only once guest work has drained.
        guard releasePeerWhenIdle, subscriptions.isEmpty, pending.isEmpty,
              subscriptionTasks.isEmpty, guestSubscriptions.isEmpty, reconnectTask == nil else { return }
        peer = nil; peerLimit = 20; blocked = false; releasePeerWhenIdle = false
    }

    private func reconnectAfterSubmittedWork(session: UUID) {
        guard reconnectTask == nil else { return }
        // Snapshot only work submitted before this boundary. Later tasks await
        // this barrier; waiting for them here would deadlock on ourselves.
        let submitted = pending.values.map(\.task) + Array(subscriptionTasks.values)
        let transport = guest, epoch = generation
        reconnectDeadline = now() + (configuration?.requestSeconds ?? 30)
        notifications.removeAll(); notificationBytes = 0; blocked = false
        reconnectTask = Task { @MainActor [weak self] in
            do {
                for task in submitted { await task.value }
                try Task.checkCancellation()
                guard let self, self.generation == epoch else { throw CancellationError() }
                try await transport.reconnect(session: session)
                try Task.checkCancellation()
                guard self.generation == epoch else { throw CancellationError() }
                if let deadline = self.reconnectDeadline, self.now() >= deadline {
                    throw BluetoothAdapterError.unavailable("Guest reconnect deadline elapsed")
                }
                self.reconnectTask = nil; self.reconnectDeadline = nil
                self.releaseIdlePeer()
            } catch {
                if let self, self.generation == epoch {
                    self.fail("Guest reconnect failed: \(error.localizedDescription)")
                }
                throw error
            }
        }
    }
    private func live() -> Bool {
        guard driver != nil else { return false }
        return state == .advertising
    }

    private func receive(_ event: BluetoothDriverEvent) {
        guard now().isFinite else { fail("Invalid monotonic time"); return }
        switch event {
        case .power(.poweredOn):
            guard state == .waitingForPower, let configuration else { return }
            state = .publishing; driver?.publish(configuration.services)
        case .power(.unknown): break
        case .power: fail("Bluetooth is not powered on or authorized")
        case .servicesPublished(let error):
            guard state == .publishing, let configuration else { return }
            if let error { fail("Bluetooth service publication failed: \(error)"); return }
            state = .advertisingPending
            driver?.advertise(localName: configuration.advertisingName, serviceUUIDs: configuration.advertisingServiceUUIDs)
        case .advertisingStarted(let error):
            guard state == .advertisingPending else { return }
            if let error { fail("Bluetooth advertising failed: \(error)") } else { state = .advertising }
        case .read(let request): read(request)
        case .write(let request): write(request)
        case .subscribed(let central, let key, let maximum):
            guard live(), attributes[key]?.notifiable == true,
                  (1...512).contains(maximum), let session = sessionID,
                  validPeer(central) else { lastError = "Subscription rejected"; return }
            releasePeerWhenIdle = false
            peerLimit = maximum
            subscriptions.insert(key)
            setSubscription(key, enabled: true, session: session)
        case .unsubscribed(let central, let key):
            guard central == peer, let session = sessionID else { return }
            subscriptions.remove(key)
            notifications.removeAll { $0.key == key }
            notificationBytes = notifications.reduce(0) { $0 + $1.value.count }
            if notifications.isEmpty { blocked = false }
            releasePeerWhenIdle = subscriptions.isEmpty
            setSubscription(key, enabled: false, session: session)
            if subscriptions.isEmpty { reconnectAfterSubmittedWork(session: session) }
        case .ready: blocked = false; drain()
        }
    }

    private func setSubscription(_ key: String, enabled: Bool, session: UUID) {
        // Reconcile desired CCCD state serially. A quick disconnect/reconnect
        // can arrive while the previous guest write is still completing.
        guard subscriptionTasks[key] == nil else { return }
        guard guestSubscriptions.contains(key) != enabled else { releaseIdlePeer(); return }
        let token = UUID(), transport = guest, epoch = generation, barrier = reconnectTask
        subscriptionTokens[key] = token
        subscriptionDeadlines[key] = now() + (configuration?.requestSeconds ?? 30)
        subscriptionTasks[key] = Task { @MainActor [weak self] in
            do {
                try await barrier?.value
                try Task.checkCancellation()
                try await transport.setNotifications(session: session, key: key, enabled: enabled)
                guard let self, self.generation == epoch, self.subscriptionTokens[key] == token else { return }
                if let deadline = self.subscriptionDeadlines[key], self.now() >= deadline {
                    self.fail("Guest subscription deadline elapsed; outcome may be unknown and session was closed")
                    return
                }
                self.subscriptionTasks[key] = nil
                self.subscriptionTokens[key] = nil
                self.subscriptionDeadlines[key] = nil
                if enabled { self.guestSubscriptions.insert(key) }
                else { self.guestSubscriptions.remove(key) }
                self.setSubscription(key, enabled: self.subscriptions.contains(key), session: session)
            } catch {
                guard let self, self.generation == epoch, self.subscriptionTokens[key] == token else { return }
                self.fail("Guest subscription failed: \(error)")
            }
        }
    }

    private func admit(_ id: UUID, peer: UUID) -> Bool {
        guard live(), validPeer(peer), pending[id] == nil else {
            driver?.respond(to: id, value: nil, result: .unlikelyError); return false
        }
        guard pending.count < 64 else {
            driver?.respond(to: id, value: nil, result: .insufficientResources); return false
        }
        return true
    }

    private func read(_ request: BluetoothReadRequest) {
        guard admit(request.id, peer: request.peer), let session = sessionID, let configuration else { return }
        guard let attribute = attributes[request.key] else { driver?.respond(to: request.id, value: nil, result: .invalidHandle); return }
        guard attribute.readable else { driver?.respond(to: request.id, value: nil, result: .readNotPermitted); return }
        guard (0...attribute.maximumLength).contains(request.offset) else { driver?.respond(to: request.id, value: nil, result: .invalidOffset); return }
        let limit = min(attribute.maximumLength - request.offset, guestMTU - 1)
        let transport = guest, epoch = generation, barrier = reconnectTask
        let task = Task { @MainActor [weak self] in
            do {
                try await barrier?.value
                try Task.checkCancellation()
                let value = try await transport.read(session: session, key: request.key, offset: request.offset, maximumLength: limit)
                self?.finish(request.id, epoch: epoch, value: value.count <= limit ? value : nil,
                             result: value.count <= limit ? .success : .invalidLength)
            } catch let error as BluetoothGuestATTError { self?.finish(request.id, epoch: epoch, value: nil, result: .guestError(error.code)) }
            catch { self?.finish(request.id, epoch: epoch, value: nil, result: .unlikelyError) }
        }
        pending[request.id] = Pending(deadline: now() + configuration.requestSeconds, mutatesGuest: false, task: task)
    }

    private func write(_ request: BluetoothWriteRequest) {
        guard admit(request.id, peer: request.peer), let session = sessionID, let configuration else { return }
        guard request.values.count == 1, let value = request.values.first else { driver?.respond(to: request.id, value: nil, result: .requestNotSupported); return }
        guard let attribute = attributes[value.key] else { driver?.respond(to: request.id, value: nil, result: .invalidHandle); return }
        guard attribute.writable else { driver?.respond(to: request.id, value: nil, result: .writeNotPermitted); return }
        guard value.offset == 0 else { driver?.respond(to: request.id, value: nil, result: .invalidOffset); return }
        guard value.value.count <= min(attribute.maximumLength, guestMTU - 3) else { driver?.respond(to: request.id, value: nil, result: .invalidLength); return }
        let transport = guest, epoch = generation, barrier = reconnectTask
        let task = Task { @MainActor [weak self] in
            do {
                try await barrier?.value
                try Task.checkCancellation()
                try await transport.write(session: session, key: value.key, value: value.value)
                self?.finish(request.id, epoch: epoch, value: nil, result: .success)
            } catch let error as BluetoothGuestATTError { self?.finish(request.id, epoch: epoch, value: nil, result: .guestError(error.code)) }
            catch { self?.finish(request.id, epoch: epoch, value: nil, result: .unlikelyError) }
        }
        pending[request.id] = Pending(deadline: now() + configuration.requestSeconds, mutatesGuest: true, task: task)
    }

    private func finish(_ id: UUID, epoch: UInt64, value: Data?, result: BluetoothATTResult) {
        guard epoch == generation, let request = pending[id] else { return }
        if request.mutatesGuest && now() >= request.deadline {
            fail("Guest operation deadline elapsed; write outcome may be unknown and session was closed")
            return
        }
        pending.removeValue(forKey: id)
        let outcome: BluetoothATTResult = now() < request.deadline ? result : .unlikelyError
        driver?.respond(to: id, value: outcome == .success ? value : nil, result: outcome)
        releaseIdlePeer()
    }

    private func receiveNotification(_ notification: BluetoothGuestNotification) {
        guard live(), reconnectTask == nil, notification.session == sessionID, let peer,
              subscriptions.contains(notification.key), let attribute = attributes[notification.key] else { return }
        let limit = min(peerLimit, guestMTU - 3, attribute.maximumLength)
        guard notification.value.count <= limit else { fail("Guest notification exceeds negotiated value limit; no truncation performed"); return }
        guard notifications.count < 64, notificationBytes + notification.value.count <= 65_536 else {
            fail("Notification queue exhausted; session stopped without claiming delivery"); return
        }
        notifications.append(Notification(key: notification.key, value: notification.value, peer: peer))
        notificationBytes += notification.value.count
        drain()
    }

    private func drain() {
        guard live() else { return }
        while !blocked, let next = notifications.first {
            guard driver?.update(next.value, characteristic: next.key, peer: next.peer) == true else { blocked = true; return }
            notifications.removeFirst(); notificationBytes -= next.value.count; acceptedByHostQueue += 1
        }
    }
}
