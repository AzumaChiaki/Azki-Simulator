import Foundation
@preconcurrency import CoreBluetooth

/// The sole radio boundary. Constructing this object does not allocate a
/// CBPeripheralManager. start() is called only by the adapter's explicit user
/// action. No discovery/central manager, HCI/USB access or credential storage.
@MainActor final class CoreBluetoothPeripheralDriver: NSObject, BluetoothPeripheralDriver, @preconcurrency CBPeripheralManagerDelegate {
    var eventHandler: ((BluetoothDriverEvent) -> Void)?
    private var manager: CBPeripheralManager?
    private var characteristics: [String: CBMutableCharacteristic] = [:]
    private var keys: [ObjectIdentifier: String] = [:]
    private var pendingServices = Set<ObjectIdentifier>()
    private var publicationFailures: [String] = []
    private var requests: [UUID: CBATTRequest] = [:]
    private var centrals: [UUID: CBCentral] = [:]
    private var centralSubscriptions: [UUID: Set<String>] = [:]

    override init() { super.init() }

    static var authorizationPending: Bool { CBManager.authorization == .notDetermined }

    func start() throws {
        guard manager == nil else { throw BluetoothAdapterError.unavailable("manager already allocated") }
        let usage = Bundle.main.object(forInfoDictionaryKey: "NSBluetoothAlwaysUsageDescription") as? String
        guard let usage, !usage.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw BluetoothAdapterError.unavailable("此应用缺少蓝牙权限说明，请使用更新后的开发版本")
        }
        guard CBManager.authorization != .denied, CBManager.authorization != .restricted else {
            throw BluetoothAdapterError.unavailable("蓝牙权限已被拒绝，请在系统设置的隐私与安全性中允许 Azki Simulator 使用蓝牙")
        }
        // Main queue makes delegate delivery serial with the @MainActor adapter.
        // macOS may show its standard Bluetooth permission prompt here.
        manager = CBPeripheralManager(delegate: self, queue: .main,
                                      options: [CBPeripheralManagerOptionShowPowerAlertKey: false])
    }

    func publish(_ services: [BluetoothServiceDefinition]) {
        do {
            try BluetoothPeripheralConfiguration(localName: "Azki BLE", guestIdentity: "service-publication", services: services).validate()
        } catch {
            eventHandler?(.servicesPublished(error: "Unsupported or invalid service schema")); return
        }
        guard let manager, manager.state == .poweredOn, characteristics.isEmpty else {
            eventHandler?(.servicesPublished(error: "Manager unavailable or already published")); return
        }
        publicationFailures.removeAll()
        var nativeServices: [CBMutableService] = []
        for service in services {
            let native = CBMutableService(type: CBUUID(string: service.uuid), primary: true)
            native.characteristics = service.characteristics.map { item in
                let properties = Self.properties(for: item)
                var permissions: CBAttributePermissions = []
                if item.readable {
                    permissions.insert(item.requiresEncryption ? .readEncryptionRequired : .readable)
                }
                if item.writable {
                    permissions.insert(item.requiresEncryption ? .writeEncryptionRequired : .writeable)
                }
                let characteristic = CBMutableCharacteristic(type: CBUUID(string: item.uuid), properties: properties,
                                                             value: nil, permissions: permissions)
                characteristics[item.key] = characteristic
                keys[ObjectIdentifier(characteristic)] = item.key
                return characteristic
            }
            pendingServices.insert(ObjectIdentifier(native))
            nativeServices.append(native)
        }
        for service in nativeServices { manager.add(service) }
    }

    static func properties(for item: BluetoothCharacteristicDefinition) -> CBCharacteristicProperties {
        var result: CBCharacteristicProperties = []
        if item.readable { result.insert(.read) }
        if item.writable {
            if item.writeWithResponse ?? !item.writeWithoutResponse { result.insert(.write) }
            if item.writeWithoutResponse { result.insert(.writeWithoutResponse) }
        }
        if item.notifiable {
            if item.notifies ?? !item.indicates {
                result.insert(.notify)
                if item.requiresEncryption { result.insert(.notifyEncryptionRequired) }
            }
            if item.indicates {
                result.insert(.indicate)
                if item.requiresEncryption { result.insert(.indicateEncryptionRequired) }
            }
        }
        return result
    }

    func advertise(localName: String, serviceUUIDs: [String]) {
        NSLog("Azki BLE advertising request: name=%@ services=%@", localName, serviceUUIDs.joined(separator: ","))
        manager?.startAdvertising([CBAdvertisementDataLocalNameKey: localName,
                                   CBAdvertisementDataServiceUUIDsKey: serviceUUIDs.map { CBUUID(string: $0) }])
    }

    func respond(to request: UUID, value: Data?, result: BluetoothATTResult) {
        guard let actual = requests.removeValue(forKey: request), let manager else { return }
        if result == .success { actual.value = value }
        manager.respond(to: actual, withResult: result.coreBluetoothCode)
    }

    func update(_ value: Data, characteristic key: String, peer: UUID) -> Bool {
        guard let manager, let characteristic = characteristics[key], let central = centrals[peer],
              value.count <= central.maximumUpdateValueLength else { return false }
        return manager.updateValue(value, for: characteristic, onSubscribedCentrals: [central])
    }

    func stop() {
        guard let manager else { return }
        for request in requests.values { manager.respond(to: request, withResult: .unlikelyError) }
        requests.removeAll()
        manager.stopAdvertising()
        manager.removeAllServices()
        manager.delegate = nil
        self.manager = nil
        characteristics.removeAll(); keys.removeAll(); pendingServices.removeAll(); centrals.removeAll(); centralSubscriptions.removeAll()
        publicationFailures.removeAll()
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        guard peripheral === manager else { return }
        let state: BluetoothPeripheralPower
        switch peripheral.state {
        case .unknown: state = .unknown
        case .resetting: state = .resetting
        case .unsupported: state = .unsupported
        case .unauthorized: state = .unauthorized
        case .poweredOff: state = .poweredOff
        case .poweredOn: state = .poweredOn
        @unknown default: state = .unsupported
        }
        eventHandler?(.power(state))
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        guard peripheral === manager, pendingServices.remove(ObjectIdentifier(service)) != nil else { return }
        if let error { publicationFailures.append("\(service.uuid.uuidString): \(Self.describe(error))") }
        if pendingServices.isEmpty {
            eventHandler?(.servicesPublished(error: publicationFailures.isEmpty ? nil : publicationFailures.joined(separator: "; ")))
        }
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        guard peripheral === manager else { return }
        NSLog("Azki BLE advertising callback: active=%@ error=%@", peripheral.isAdvertising.description, error.map(Self.describe) ?? "none")
        eventHandler?(.advertisingStarted(error: error.map(Self.describe)))
    }

    static func describe(_ error: Error) -> String {
        let error = error as NSError
        return "\(error.domain) (\(error.code)): \(error.localizedDescription)"
    }

    private func store(_ request: CBATTRequest, manager: CBPeripheralManager) -> UUID? {
        guard requests.count < 64 else { manager.respond(to: request, withResult: .insufficientResources); return nil }
        let id = UUID(); requests[id] = request; return id
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        guard peripheral === manager else { return }
        guard let key = keys[ObjectIdentifier(request.characteristic)] else {
            peripheral.respond(to: request, withResult: .invalidHandle); return
        }
        guard let id = store(request, manager: peripheral) else { return }
        eventHandler?(.read(BluetoothReadRequest(id: id, peer: request.central.identifier, key: key, offset: request.offset)))
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        guard peripheral === manager, let first = requests.first else { return }
        // Never partially commit an ATT prepared/reliable write batch.
        guard requests.count == 1 else { peripheral.respond(to: first, withResult: .requestNotSupported); return }
        guard let key = keys[ObjectIdentifier(first.characteristic)] else {
            peripheral.respond(to: first, withResult: .invalidHandle); return
        }
        guard let id = store(first, manager: peripheral) else { return }
        eventHandler?(.write(BluetoothWriteRequest(id: id, peer: first.central.identifier,
                    values: [BluetoothWriteValue(key: key, offset: first.offset, value: first.value ?? Data())])))
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic) {
        guard peripheral === manager, let key = keys[ObjectIdentifier(characteristic)] else { return }
        // The adapter owns admission. A rejected second subscriber must not
        // prevent the admitted read/write peer from subscribing afterwards.
        // Retaining a CBCentral here does not admit it or send it guest data.
        guard centrals.count < 64 || centrals[central.identifier] != nil else { return }
        centrals[central.identifier] = central
        centralSubscriptions[central.identifier, default: []].insert(key)
        eventHandler?(.subscribed(peer: central.identifier, key: key, maximumValueLength: central.maximumUpdateValueLength))
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom characteristic: CBCharacteristic) {
        guard peripheral === manager, let key = keys[ObjectIdentifier(characteristic)] else { return }
        centralSubscriptions[central.identifier]?.remove(key)
        if centralSubscriptions[central.identifier]?.isEmpty == true {
            centralSubscriptions[central.identifier] = nil
            centrals[central.identifier] = nil
        }
        eventHandler?(.unsubscribed(peer: central.identifier, key: key))
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        guard peripheral === manager else { return }
        eventHandler?(.ready)
    }
}

private extension BluetoothATTResult {
    var coreBluetoothCode: CBATTError.Code {
        switch self {
        case .guestError(let code): return CBATTError.Code(rawValue: code) ?? .unlikelyError
        case .success: return .success
        case .invalidHandle: return .invalidHandle
        case .readNotPermitted: return .readNotPermitted
        case .writeNotPermitted: return .writeNotPermitted
        case .invalidOffset: return .invalidOffset
        case .invalidLength: return .invalidAttributeValueLength
        case .insufficientResources: return .insufficientResources
        case .unlikelyError: return .unlikelyError
        case .requestNotSupported: return .requestNotSupported
        }
    }
}
