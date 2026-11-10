using System.Collections.Concurrent;
using System.Globalization;
using System.Text.Json;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Foundation;
using Windows.Storage.Streams;

namespace AzkiBluetooth;

/// WinRT owns the physical links. This is not a raw HCI controller or an SMP proxy.
public sealed class RadioHost : IDisposable
{
    private readonly object outputLock = new();
    private readonly SemaphoreSlim centralOperations = new(1, 1);
    private readonly SemaphoreSlim peripheralOperations = new(1, 1);
    private readonly SemaphoreSlim radioOperations = new(1, 1);
    private readonly CancellationTokenSource lifetime = new();
    private BluetoothLEAdvertisementWatcher? watcher;
    private BluetoothLEAdvertisementPublisher? publisher;
    private readonly Dictionary<string, BluetoothLEDevice> devices = [];
    private readonly Dictionary<string, Dictionary<string, GattCharacteristic>> remoteChars = [];
    private readonly Dictionary<string, Dictionary<string, GattDescriptor>> remoteDescriptors = [];
    private readonly Dictionary<GattCharacteristic, TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>> subscriptionHandlers = [];
    private readonly Dictionary<string, TypedEventHandler<BluetoothLEDevice, object>> connectionHandlers = [];
    private readonly Dictionary<string, List<GattDeviceService>> remoteServices = [];
    private readonly List<GattServiceProvider> providers = [];
    private readonly Dictionary<string, GattLocalCharacteristic> localChars = [];
    private readonly List<Action> detachLocalEvents = [];
    private readonly ConcurrentDictionary<string, TaskCompletionSource<AttReply>> pending = new();
    private readonly SemaphoreSlim pendingSlots = new(64, 64);
    private readonly HashSet<string> subscribedPeers = [];
    private volatile int peripheralGeneration;
    private bool disposed;
    private sealed record AttReply(byte Error, byte[] Value);
    private static readonly HashSet<ushort> ReservedServices = [0x1800, 0x1801, 0x180a, 0x1813];
    private static readonly HashSet<int> ReservedAdvertisingTypes = [
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x1e,0x1f,0x20,0x21,0x3d];

    public void Write(object value)
    {
        lock (outputLock) { Console.Out.WriteLine(JsonSerializer.Serialize(value)); Console.Out.Flush(); }
    }
    private void Event(string name, object value) => Write(new { @event = name, data = value });
    private static string Text(JsonElement r, string key, string? fallback = null) =>
        r.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! :
        fallback ?? throw new ArgumentException($"Missing string {key}");
    private static bool Flag(JsonElement r, string key, bool fallback = false) =>
        r.TryGetProperty(key, out var v) ? v.ValueKind switch {
            JsonValueKind.True => true, JsonValueKind.False => false,
            _ => throw new ArgumentException($"Expected boolean {key}") } : fallback;
    private static int Number(JsonElement r, string key, int fallback, int low, int high)
    {
        var value = r.TryGetProperty(key, out var v) ? v.GetInt32() : fallback;
        return value >= low && value <= high ? value : throw new ArgumentException($"Invalid {key}");
    }
    private static byte[] Bytes(JsonElement r, string key = "value", int max = 512)
    {
        var text = Text(r, key, "");
        if (text.Length > max * 2 || text.Length % 2 != 0) throw new ArgumentException($"Invalid {key} length");
        return Convert.FromHexString(text);
    }
    private static IBuffer Buffer(byte[] bytes) { using var w = new DataWriter(); w.WriteBytes(bytes); return w.DetachBuffer(); }
    private static byte[] Bytes(IBuffer buffer) { using var r = DataReader.FromBuffer(buffer); var b = new byte[buffer.Length]; r.ReadBytes(b); return b; }
    private static Guid Uuid(string value) => value.Length switch {
        4 => Guid.Parse($"0000{value}-0000-1000-8000-00805f9b34fb"),
        8 => Guid.Parse($"{value}-0000-1000-8000-00805f9b34fb"),
        _ => Guid.Parse(value) };
    private static ulong Address(string value)
    {
        var hex = value.Replace(":", "").Replace("-", "");
        if (hex.Length != 12) throw new ArgumentException("Expected 48-bit Bluetooth address");
        return ulong.Parse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    }
    private static string Address(ulong value) => value.ToString("X12", CultureInfo.InvariantCulture);
    private static void Ensure(GattCommunicationStatus status, byte? protocol = null)
    {
        if (status != GattCommunicationStatus.Success)
            throw new InvalidOperationException($"GATT {status}; ATT error={protocol?.ToString(CultureInfo.InvariantCulture) ?? "none"}");
    }
    private CancellationTokenSource Deadline(JsonElement r)
    {
        var milliseconds = Number(r, "timeoutMs", 15000, 100, 60000);
        var deadline = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        deadline.CancelAfter(milliseconds);
        return deadline;
    }

    public async Task HandleAsync(JsonElement request)
    {
        string? id = request.TryGetProperty("id", out var rawId) && rawId.ValueKind == JsonValueKind.String ? rawId.GetString() : null;
        try
        {
            if (id is null || id.Length is < 1 or > 128) throw new ArgumentException("A bounded string id is required");
            var command = Text(request, "command");
            using var deadline = Deadline(request);
            object result;
            if (command == "peripheral.respond") result = await DispatchAsync(command, request, deadline.Token);
            else
            {
                var gate = command.StartsWith("central.", StringComparison.Ordinal) ? centralOperations :
                    command.StartsWith("peripheral.", StringComparison.Ordinal) ? peripheralOperations : radioOperations;
                await gate.WaitAsync(deadline.Token);
                try { result = await DispatchAsync(command, request, deadline.Token); }
                finally { gate.Release(); }
            }
            Write(new { id, ok = true, result });
        }
        catch (Exception error)
        {
            Write(new { id, ok = false, error = new {
                code = error is OperationCanceledException ? "timeout" : error is ArgumentException or FormatException ? "invalid_request" : "bluetooth_error",
                message = error.Message, hresult = $"0x{error.HResult:X8}" } });
        }
    }

    private async Task<object> DispatchAsync(string command, JsonElement r, CancellationToken ct)
    {
        switch (command)
        {
            case "adapters":
            {
                var found = await DeviceInformation.FindAllAsync(BluetoothAdapter.GetDeviceSelector()).AsTask(ct);
                var result = new List<object>();
                foreach (var item in found)
                {
                    var adapter = await BluetoothAdapter.FromIdAsync(item.Id).AsTask(ct);
                    if (adapter is not null) result.Add(new { name = item.Name, deviceID = item.Id,
                        lowEnergy = adapter.IsLowEnergySupported, central = adapter.IsCentralRoleSupported,
                        peripheral = adapter.IsPeripheralRoleSupported, extendedAdvertising = adapter.IsExtendedAdvertisingSupported });
                }
                return new { adapters = result, selection = "WinRT watcher/publisher/GATT APIs use Windows-selected adapter; raw dedicated-controller transport is separate" };
            }
            case "status":
            {
                var a = await BluetoothAdapter.GetDefaultAsync().AsTask(ct);
                if (a is null) return new { available = false, rawHci = false, reason = "No Windows Bluetooth adapter" };
                var radio = await a.GetRadioAsync().AsTask(ct);
                return new { available = true, radioState = radio.State.ToString(),
                    lowEnergy = a.IsLowEnergySupported, central = a.IsCentralRoleSupported,
                    peripheral = a.IsPeripheralRoleSupported, classic = a.IsClassicSupported,
                    extendedAdvertising = a.IsExtendedAdvertisingSupported,
                    maximumAdvertisementDataLength = a.MaxAdvertisementDataLength,
                    scanning = watcher?.Status.ToString() ?? "Stopped", advertising = publisher?.Status.ToString() ?? "Stopped",
                    serviceProviders = providers.Count, rawHci = false, physicalLinkSecurity = "managed-by-Windows",
                    arbitraryFirmwareAdvertisingPayload = false, sco = false, iso = false,
                    reservedServiceUUIDs = new[] { "1800", "1801", "180a", "1813" },
                    reservedAdvertisementDataTypes = ReservedAdvertisingTypes.Order().ToArray() };
            }
            case "scan.start":
                if (watcher is not null) throw new InvalidOperationException("This host is already scanning");
                watcher = new BluetoothLEAdvertisementWatcher {
                    ScanningMode = Flag(r, "active") ? BluetoothLEScanningMode.Active : BluetoothLEScanningMode.Passive,
                    AllowExtendedAdvertisements = Flag(r, "extended") };
                if (r.TryGetProperty("serviceUUIDs", out var filter))
                    foreach (var u in filter.EnumerateArray()) watcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(Uuid(u.GetString()!));
                watcher.Received += AdvertisementReceived;
                watcher.Stopped += (_, e) => Event("scan.stopped", new { error = e.Error.ToString() });
                watcher.Start();
                return new { state = watcher.Status.ToString() };
            case "scan.stop": StopScan(); return new { stopped = true };
            case "advertise.start":
            {
                if (publisher is not null) throw new InvalidOperationException("This host already owns an advertisement");
                var ad = new BluetoothLEAdvertisement();
                if (r.TryGetProperty("manufacturer", out var manufacture))
                    foreach (var entry in manufacture.EnumerateArray())
                        ad.ManufacturerData.Add(new BluetoothLEManufacturerData((ushort)Number(entry, "companyID", 0, 0, 65535), Buffer(Bytes(entry, "value", 254))));
                if (r.TryGetProperty("sections", out var sections))
                    foreach (var entry in sections.EnumerateArray())
                    {
                        var type = Number(entry, "type", -1, 0, 255);
                        if (ReservedAdvertisingTypes.Contains(type)) throw new ArgumentException($"AD type 0x{type:X2} is reserved by Windows; payload was not changed");
                        ad.DataSections.Add(new BluetoothLEAdvertisementDataSection((byte)type, Buffer(Bytes(entry, "value", 254))));
                    }
                if (ad.ManufacturerData.Count == 0 && ad.DataSections.Count == 0) throw new ArgumentException("An advertisement payload is required");
                var p = new BluetoothLEAdvertisementPublisher(ad) { UseExtendedAdvertisement = Flag(r, "extended") };
                p.StatusChanged += (_, e) => Event("advertise.status", new { state = e.Status.ToString(), error = e.Error.ToString() });
                publisher = p;
                p.Start();
                return new { state = p.Status.ToString(), transmissionVerified = false };
            }
            case "advertise.stop": StopAdvertising(); return new { stopped = true };
            case "central.connect":
            {
                if (devices.Count >= 8) throw new InvalidOperationException("At most 8 owned central devices");
                var type = Text(r, "addressType", "unspecified") switch {
                    "public" => BluetoothAddressType.Public, "random" => BluetoothAddressType.Random,
                    "unspecified" => BluetoothAddressType.Unspecified, _ => throw new ArgumentException("Invalid addressType") };
                var device = await BluetoothLEDevice.FromBluetoothAddressAsync(Address(Text(r, "address")), type).AsTask(ct);
                if (device is null) throw new InvalidOperationException("Windows did not resolve the selected Bluetooth device");
                var identity = Guid.NewGuid().ToString();
                try
                {
                    var services = await device.GetGattServicesAsync(BluetoothCacheMode.Uncached).AsTask(ct);
                    Ensure(services.Status, services.ProtocolError);
                    foreach (var service in services.Services) service.Dispose();
                    devices.Add(identity, device); remoteChars.Add(identity, []); remoteDescriptors.Add(identity, []); remoteServices.Add(identity, []);
                    TypedEventHandler<BluetoothLEDevice, object> changed = (_, _) => Event("central.connection", new { deviceID = identity, state = device.ConnectionStatus.ToString() });
                    device.ConnectionStatusChanged += changed;
                    connectionHandlers.Add(identity, changed);
                    return new { deviceID = identity, state = device.ConnectionStatus.ToString(), paired = device.DeviceInformation.Pairing.IsPaired };
                }
                catch { device.Dispose(); throw; }
            }
            case "central.discover":
            {
                var identity = Text(r, "deviceID"); var device = Device(identity);
                if (remoteChars[identity].Count != 0) throw new InvalidOperationException("Discovery already completed; disconnect before rediscovery");
                var discovered = await device.GetGattServicesAsync(BluetoothCacheMode.Uncached).AsTask(ct); Ensure(discovered.Status, discovered.ProtocolError);
                var result = new List<object>();
                foreach (var s in discovered.Services)
                {
                    remoteServices[identity].Add(s);
                    var chars = await s.GetCharacteristicsAsync(BluetoothCacheMode.Uncached).AsTask(ct); Ensure(chars.Status, chars.ProtocolError);
                    foreach (var c in chars.Characteristics) remoteChars[identity].Add("h" + c.AttributeHandle, c);
                    result.Add(new { key = "s" + s.AttributeHandle, uuid = s.Uuid.ToString(), characteristics = chars.Characteristics.Select(c => new {
                        key = "h" + c.AttributeHandle, uuid = c.Uuid.ToString(), properties = (uint)c.CharacteristicProperties,
                        readable = c.CharacteristicProperties.HasFlag(GattCharacteristicProperties.Read),
                        writable = (c.CharacteristicProperties & (GattCharacteristicProperties.Write | GattCharacteristicProperties.WriteWithoutResponse)) != 0,
                        notifiable = (c.CharacteristicProperties & (GattCharacteristicProperties.Notify | GattCharacteristicProperties.Indicate)) != 0 }) });
                }
                return new { services = result };
            }
            case "central.read":
            {
                var c = Characteristic(r); var value = await c.ReadValueAsync(BluetoothCacheMode.Uncached).AsTask(ct); Ensure(value.Status, value.ProtocolError);
                return new { value = Convert.ToHexString(Bytes(value.Value)).ToLowerInvariant() };
            }
            case "central.write":
            {
                var c = Characteristic(r); var mode = Flag(r, "withResponse", true) ? GattWriteOption.WriteWithResponse : GattWriteOption.WriteWithoutResponse;
                var written = await c.WriteValueWithResultAsync(Buffer(Bytes(r)), mode).AsTask(ct); Ensure(written.Status, written.ProtocolError);
                return new { submitted = true, withResponse = mode == GattWriteOption.WriteWithResponse };
            }
            case "central.subscribe":
            {
                var c = Characteristic(r); var identity = Text(r, "deviceID"); var key = Text(r, "key"); var enabled = Flag(r, "enabled", true);
                var mode = !enabled ? GattClientCharacteristicConfigurationDescriptorValue.None :
                    Flag(r, "indicate") ? GattClientCharacteristicConfigurationDescriptorValue.Indicate : GattClientCharacteristicConfigurationDescriptorValue.Notify;
                TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>? addedHandler = null;
                if (enabled && !subscriptionHandlers.ContainsKey(c))
                {
                    addedHandler = (_, args) => Event("central.notification", new { deviceID = identity, key,
                        value = Convert.ToHexString(Bytes(args.CharacteristicValue)).ToLowerInvariant() });
                    c.ValueChanged += addedHandler;
                    subscriptionHandlers.Add(c, addedHandler);
                }
                try
                {
                    var status = await c.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(mode).AsTask(ct); Ensure(status.Status, status.ProtocolError);
                }
                catch
                {
                    if (addedHandler is not null) { c.ValueChanged -= addedHandler; subscriptionHandlers.Remove(c); }
                    throw;
                }
                if (!enabled && subscriptionHandlers.Remove(c, out var oldHandler)) c.ValueChanged -= oldHandler;
                return new { subscribed = enabled };
            }
            case "central.descriptors":
            {
                var identity = Text(r, "deviceID"); var descriptors = await Characteristic(r).GetDescriptorsAsync(BluetoothCacheMode.Uncached).AsTask(ct); Ensure(descriptors.Status, descriptors.ProtocolError);
                foreach (var d in descriptors.Descriptors) remoteDescriptors[identity]["d" + d.AttributeHandle] = d;
                return new { descriptors = descriptors.Descriptors.Select(d => new { key = "d" + d.AttributeHandle, uuid = d.Uuid.ToString() }) };
            }
            case "central.read-descriptor":
            {
                var d = Descriptor(r); var value = await d.ReadValueAsync(BluetoothCacheMode.Uncached).AsTask(ct); Ensure(value.Status, value.ProtocolError);
                return new { value = Convert.ToHexString(Bytes(value.Value)).ToLowerInvariant() };
            }
            case "central.write-descriptor":
            {
                var written = await Descriptor(r).WriteValueWithResultAsync(Buffer(Bytes(r))).AsTask(ct); Ensure(written.Status, written.ProtocolError); return new { submitted = true };
            }
            case "central.disconnect": Disconnect(Text(r, "deviceID")); return new { releasedOwnedDevice = true, systemWideDisconnect = false };
            case "peripheral.publish": return await PublishAsync(r, ct);
            case "peripheral.start":
                if (providers.Count == 0) throw new InvalidOperationException("Publish a service before advertising");
                foreach (var p in providers) p.StartAdvertising(new GattServiceProviderAdvertisingParameters {
                    IsConnectable = Flag(r, "connectable", true), IsDiscoverable = Flag(r, "discoverable", true) });
                return new { requested = true, serviceProviders = providers.Count, transmissionVerified = false };
            case "peripheral.respond":
            {
                var operation = Text(r, "operationID");
                var reply = new AttReply((byte)Number(r, "attError", 0, 0, 255), Bytes(r));
                if (!pending.TryRemove(operation, out var p)) throw new InvalidOperationException("Stale or expired peripheral request");
                p.TrySetResult(reply);
                return new { accepted = true };
            }
            case "peripheral.notify":
            {
                var key = Text(r, "key");
                if (!localChars.TryGetValue(key, out var c)) throw new ArgumentException("Unknown local characteristic");
                var value = Bytes(r); var peer = Text(r, "peerID", "");
                if (peer.Length == 0) throw new ArgumentException("An admitted peerID is required for notification");
                var client = c.SubscribedClients.FirstOrDefault(p => Peer(p.Session) == peer) ?? throw new InvalidOperationException("Peer is not subscribed");
                if (value.Length > client.MaxNotificationSize) throw new ArgumentException("Value exceeds peer notification limit; no truncation performed");
                var result = await c.NotifyValueAsync(Buffer(value), client).AsTask(ct);
                Ensure(result.Status, result.ProtocolError);
                return new { submitted = true, status = result.Status.ToString(), applicationAcknowledgement = false };
            }
            case "peripheral.stop": StopPeripheral(); return new { stopped = true };
            default: throw new ArgumentException("Unknown Bluetooth host command");
        }
    }

    private BluetoothLEDevice Device(string identity) => devices.TryGetValue(identity, out var d) ? d : throw new ArgumentException("Unknown owned deviceID");
    private GattCharacteristic Characteristic(JsonElement r)
    {
        var id = Text(r, "deviceID"); _ = Device(id);
        return remoteChars[id].TryGetValue(Text(r, "key"), out var c) ? c : throw new ArgumentException("Unknown discovered characteristic");
    }
    private GattDescriptor Descriptor(JsonElement r)
    {
        var id = Text(r, "deviceID"); _ = Device(id);
        return remoteDescriptors[id].TryGetValue(Text(r, "key"), out var d) ? d : throw new ArgumentException("Unknown discovered descriptor");
    }
    private void AdvertisementReceived(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementReceivedEventArgs e)
    {
        if (sender != watcher || disposed) return;
        Event("scan.received", new { address = Address(e.BluetoothAddress), addressType = e.BluetoothAddressType.ToString().ToLowerInvariant(),
            rssi = e.RawSignalStrengthInDBm, kind = e.AdvertisementType.ToString(), name = e.Advertisement.LocalName,
            serviceUUIDs = e.Advertisement.ServiceUuids.Select(g => g.ToString()),
            manufacturer = e.Advertisement.ManufacturerData.Select(m => new { companyID = m.CompanyId, value = Convert.ToHexString(Bytes(m.Data)).ToLowerInvariant() }),
            sections = e.Advertisement.DataSections.Select(s => new { type = s.DataType, value = Convert.ToHexString(Bytes(s.Data)).ToLowerInvariant() }) });
    }
    private async Task<object> PublishAsync(JsonElement r, CancellationToken ct)
    {
        if (providers.Count != 0) throw new InvalidOperationException("Stop the previous peripheral before publication");
        var services = r.GetProperty("services").EnumerateArray().ToArray();
        if (services.Length is < 1 or > 64) throw new ArgumentException("Expected 1..64 services");
        var parsed = new List<(Guid Uuid, JsonElement[] Chars)>(); var keys = new HashSet<string>(); var count = 0;
        foreach (var s in services)
        {
            var guid = Uuid(Text(s, "uuid"));
            if (ReservedServices.Any(u => guid == Uuid(u.ToString("x4")))) throw new ArgumentException($"Service {guid} is reserved by Windows; no partial publication performed");
            var chars = s.GetProperty("characteristics").EnumerateArray().ToArray();
            if (chars.Length == 0) throw new ArgumentException("Service has no characteristics");
            foreach (var c in chars)
            {
                if (!keys.Add(Text(c, "key")) || ++count > 256) throw new ArgumentException("Duplicate characteristic key or more than256 characteristics");
                _ = Uuid(Text(c, "uuid"));
                if (!Flag(c, "readable") && !Flag(c, "writable") && !Flag(c, "notifiable")) throw new ArgumentException("Characteristic has no supported operation");
            }
            parsed.Add((guid, chars));
        }
        var generation = ++peripheralGeneration;
        try
        {
            foreach (var s in parsed)
            {
                var result = await GattServiceProvider.CreateAsync(s.Uuid).AsTask(ct);
                if (result.Error != BluetoothError.Success) throw new InvalidOperationException($"Service publication: {result.Error}");
                var provider = result.ServiceProvider; providers.Add(provider);
                TypedEventHandler<GattServiceProvider, GattServiceProviderAdvertisementStatusChangedEventArgs> advertisementChanged =
                    (_, e) => Event("peripheral.advertising", new { serviceUUID = s.Uuid, state = e.Status.ToString(), error = e.Error.ToString() });
                provider.AdvertisementStatusChanged += advertisementChanged;
                detachLocalEvents.Add(() => provider.AdvertisementStatusChanged -= advertisementChanged);
                foreach (var c in s.Chars)
                {
                    var key = Text(c, "key"); var properties = GattCharacteristicProperties.None;
                    if (Flag(c, "readable")) properties |= GattCharacteristicProperties.Read;
                    if (Flag(c, "writable"))
                    {
                        if (Flag(c, "writeWithResponse", !Flag(c, "writeWithoutResponse"))) properties |= GattCharacteristicProperties.Write;
                        if (Flag(c, "writeWithoutResponse")) properties |= GattCharacteristicProperties.WriteWithoutResponse;
                    }
                    if (Flag(c, "notifiable"))
                    {
                        if (Flag(c, "notifies", !Flag(c, "indicates"))) properties |= GattCharacteristicProperties.Notify;
                        if (Flag(c, "indicates")) properties |= GattCharacteristicProperties.Indicate;
                    }
                    var protection = Flag(c, "requiresEncryption") ? GattProtectionLevel.EncryptionRequired : GattProtectionLevel.Plain;
                    var created = await provider.Service.CreateCharacteristicAsync(Uuid(Text(c, "uuid")), new GattLocalCharacteristicParameters {
                        CharacteristicProperties = properties, ReadProtectionLevel = protection, WriteProtectionLevel = protection }).AsTask(ct);
                    if (created.Error != BluetoothError.Success) throw new InvalidOperationException($"Characteristic publication: {created.Error}");
                    var characteristic = created.Characteristic; localChars.Add(key, characteristic);
                    TypedEventHandler<GattLocalCharacteristic, GattReadRequestedEventArgs> read = (sender, args) => ReadRequested(sender, args, key, generation);
                    TypedEventHandler<GattLocalCharacteristic, GattWriteRequestedEventArgs> write = (sender, args) => WriteRequested(sender, args, key, generation);
                    TypedEventHandler<GattLocalCharacteristic, object> subscription = (sender, _) => SubscriptionsChanged(sender, key, generation);
                    characteristic.ReadRequested += read;
                    characteristic.WriteRequested += write;
                    characteristic.SubscribedClientsChanged += subscription;
                    detachLocalEvents.Add(() => {
                        characteristic.ReadRequested -= read; characteristic.WriteRequested -= write;
                        characteristic.SubscribedClientsChanged -= subscription;
                    });
                }
            }
            return new { published = true, services = providers.Count, characteristics = localChars.Count, generation,
                localNameSet = false, physicalLinkSecurity = "managed-by-Windows", rawHci = false };
        }
        catch { StopPeripheral(); throw; }
    }
    private static string Peer(GattSession session) => session.DeviceId.Id;
    private async Task<AttReply> RequestAsync(string operation, string peer, string key, int offset, byte[] value, uint maximumLength, int generation)
    {
        if (generation != peripheralGeneration || disposed) return new AttReply(0x0e, []);
        if (!pendingSlots.Wait(0)) return new AttReply(0x11, []);
        var id = Guid.NewGuid().ToString(); var completion = new TaskCompletionSource<AttReply>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!pending.TryAdd(id, completion)) { pendingSlots.Release(); return new AttReply(0x11, []); }
        Event("peripheral.request", new { operationID = id, operation, peerID = peer, key, offset, generation,
            value = Convert.ToHexString(value).ToLowerInvariant(), maximumLength });
        try { return await completion.Task.WaitAsync(TimeSpan.FromSeconds(20), lifetime.Token); }
        catch { return new AttReply(0x0e, []); }
        finally { pending.TryRemove(id, out _); pendingSlots.Release(); }
    }
    private async void ReadRequested(GattLocalCharacteristic sender, GattReadRequestedEventArgs args, string key, int generation)
    {
        var deferral = args.GetDeferral();
        try
        {
            var request = await args.GetRequestAsync(); if (request is null) return;
            var reply = await RequestAsync("read", Peer(args.Session), key, (int)request.Offset, [], request.Length, generation);
            if (reply.Error != 0) request.RespondWithProtocolError(reply.Error);
            else if (reply.Value.Length > request.Length) request.RespondWithProtocolError(0x0d);
            else request.RespondWithValue(Buffer(reply.Value));
        }
        catch (Exception e) { Event("peripheral.error", new { operation = "read", message = e.Message }); }
        finally { deferral.Complete(); }
    }
    private async void WriteRequested(GattLocalCharacteristic sender, GattWriteRequestedEventArgs args, string key, int generation)
    {
        var deferral = args.GetDeferral();
        try
        {
            var request = await args.GetRequestAsync(); if (request is null) return;
            // The existing guest ATT proxy has no prepared/reliable write API.
            var reply = request.Offset == 0 ? await RequestAsync("write", Peer(args.Session), key, 0, Bytes(request.Value), 0, generation) : new AttReply(0x07, []);
            if (request.Option == GattWriteOption.WriteWithResponse)
            { if (reply.Error != 0) request.RespondWithProtocolError(reply.Error); else request.Respond(); }
            else if (reply.Error != 0) Event("peripheral.write_failed", new { key, attError = reply.Error, responsePossible = false });
        }
        catch (Exception e) { Event("peripheral.error", new { operation = "write", message = e.Message }); }
        finally { deferral.Complete(); }
    }
    private void SubscriptionsChanged(GattLocalCharacteristic sender, string key, int generation)
    {
        if (generation != peripheralGeneration || disposed) return;
        var current = sender.SubscribedClients.Select(c => (Token: key + "\0" + Peer(c.Session), Peer: Peer(c.Session), Limit: c.MaxNotificationSize)).ToArray();
        lock (subscribedPeers)
        {
            foreach (var old in subscribedPeers.Where(p => p.StartsWith(key + "\0", StringComparison.Ordinal) && !current.Any(c => c.Token == p)).ToArray())
            { subscribedPeers.Remove(old); Event("peripheral.subscription", new { key, peerID = old[(key.Length + 1)..], enabled = false, maximumLength = 0, generation }); }
            foreach (var item in current)
                if (subscribedPeers.Add(item.Token)) Event("peripheral.subscription", new { key, peerID = item.Peer, enabled = true, maximumLength = item.Limit, generation });
        }
    }
    private void StopScan()
    { var w = watcher; watcher = null; if (w is not null) { w.Received -= AdvertisementReceived; w.Stop(); } }
    private void StopAdvertising()
    { var p = publisher; publisher = null; p?.Stop(); }
    private void Disconnect(string identity)
    {
        if (!devices.Remove(identity, out var device)) throw new ArgumentException("Unknown owned deviceID");
        if (connectionHandlers.Remove(identity, out var changed)) device.ConnectionStatusChanged -= changed;
        foreach (var c in remoteChars[identity].Values)
            if (subscriptionHandlers.Remove(c, out var handler)) c.ValueChanged -= handler;
        foreach (var s in remoteServices[identity]) s.Dispose();
        remoteServices.Remove(identity);
        remoteChars.Remove(identity); remoteDescriptors.Remove(identity); device.Dispose();
    }
    private void StopPeripheral()
    {
        ++peripheralGeneration;
        foreach (var p in providers) p.StopAdvertising();
        foreach (var detach in detachLocalEvents) detach();
        detachLocalEvents.Clear();
        providers.Clear(); localChars.Clear(); lock (subscribedPeers) subscribedPeers.Clear();
        foreach (var operation in pending.Keys) if (pending.TryRemove(operation, out var completion)) completion.TrySetResult(new AttReply(0x0e, []));
    }
    public void Dispose()
    {
        if (disposed) return; disposed = true;
        RequestShutdown();
        StopScan(); StopAdvertising(); StopPeripheral();
        foreach (var id in devices.Keys.ToArray()) Disconnect(id);
    }
    public void RequestShutdown()
    {
        lifetime.Cancel();
        foreach (var operation in pending.Keys)
            if (pending.TryRemove(operation, out var completion)) completion.TrySetResult(new AttReply(0x0e, []));
    }
}
