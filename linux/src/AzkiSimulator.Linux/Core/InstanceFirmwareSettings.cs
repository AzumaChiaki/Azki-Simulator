using System.Text.Json;
using System.Text.Json.Nodes;
namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Port of InstanceFirmwareSettings.swift: per-instance board settings the
/// helper stores under the firmware library and writes into persist.db at the
/// next boot. Decoding refuses anything the Swift receipt validator would
/// refuse, so a damaged file never reaches an editor.
/// </summary>
public sealed class InstanceFirmwareSettings
{
    // Swift's Int is 64-bit; this bound exceeds int.MaxValue, so it stays long here.
    public const long MaximumOffsetSeconds = 100L * 366 * 86_400;
    public const int MaximumSampleRaw = 65_535;
    public const int MaximumSteps = 10_000_000;
    public const int MaximumHeartRate = 255;
    public const int RestingHeartRate = 72;
    public static readonly (int Min, int Max) HeartRateRange = (30, 220);
    public static readonly (int Min, int Max) CadenceRange = (40, 200);
    public static readonly (int Min, int Max) AddStepsRange = (1, 100_000);
    public static readonly (int Min, int Max) BatteryLevelRange = (0, 100);
    public static readonly (double Min, double Max) TemperatureRange = (-40, 85);
    public static readonly string[] Chargers = ["auto", "unplugged", "charging", "full"];

    public int Schema = 1;
    public Dictionary<string, string?> Properties = new();
    public string ClockMode = "host";
    public int? ClockOffsetSeconds;
    public BatterySettings? Battery;
    public HealthSettings? Health;

    public sealed class BatterySettings
    {
        public int? Level;
        public int? SampleRaw;
        public string Charger = "auto";
        public string Gauge = "auto";

        public JsonObject Encode()
        {
            // The helper's validator requires exact key sets, and a JsonObject
            // indexer assigned null keeps the key (proven by a probe), so absent
            // fields must be omitted rather than written as null.
            var value = new JsonObject { ["charger"] = Charger, ["gauge"] = Gauge };
            if (Level is { } level) value["level"] = level;
            if (SampleRaw is { } raw) value["sampleRaw"] = raw;
            return value;
        }
    }

    /// <summary>An explicit "off" state is absent so the emulated default is kept.</summary>
    public sealed class HealthSettings
    {
        public int? WalkSteps;
        public int? Cadence;
        public int? HeartRate;
        public bool? Worn;
        public bool IsEmpty => WalkSteps is null && Cadence is null && HeartRate is null && Worn is null;

        public JsonObject Encode()
        {
            var value = new JsonObject();
            if (WalkSteps is { } steps) value["walkSteps"] = steps;
            if (Cadence is { } cadence) value["cadence"] = cadence;
            if (HeartRate is { } heartRate) value["heartRate"] = heartRate;
            if (Worn is { } worn) value["worn"] = worn;
            return value;
        }
    }

    public JsonObject Encode()
    {
        // A null property value is an explicit reset and must survive encoding.
        // The JsonObject indexer drops keys assigned null, and the resolved
        // System.Text.Json here has no JsonNull node type, so the object is
        // parsed from literal JSON where null is representable.
        var properties = "{" + string.Join(",", Properties.Select(pair =>
            JsonSerializer.Serialize(pair.Key) + ":" + (pair.Value is null ? "null" : JsonSerializer.Serialize(pair.Value)))) + "}";
        // Non-offset clocks must carry exactly {"mode"}; see the battery note above.
        var clock = new JsonObject { ["mode"] = ClockMode };
        if (ClockMode == "offset" && ClockOffsetSeconds is { } offset) clock["offsetSeconds"] = offset;
        var document = new JsonObject
        {
            ["schema"] = Schema,
            ["properties"] = JsonNode.Parse(properties)!.AsObject(),
            ["clock"] = clock,
        };
        if (Battery is { } battery) document["battery"] = battery.Encode();
        if (Health is { } health && !health.IsEmpty) document["health"] = health.Encode();
        return document;
    }

    public static bool ValidKey(string key) =>
        key.StartsWith("persist.", StringComparison.Ordinal) && key.Length > 8
        && System.Text.Encoding.UTF8.GetByteCount(key) <= 128
        && key.All(character => character is >= '0' and <= '9' or >= 'A' and <= 'Z' or >= 'a' and <= 'z'
            or '-' or '.' or '_');

    public static bool ValidValue(string value) =>
        value.Length <= 1_024 && !value.Any(character => character is '\0' or '\n' or '\r' or '\t');

    /// <summary>Decode with the same guards as InstanceFirmwareSettingsReceipt.decode;
    /// throws InvalidDataException instead of trusting a damaged file into the UI.</summary>
    public static InstanceFirmwareSettings Decode(JsonNode? node)
    {
        if (node is not JsonObject document) throw new InvalidDataException("实例固件设置不是对象");
        if (JsonRead.AsInt(document["schema"], -1) != 1) throw new InvalidDataException("实例固件设置版本不受支持");
        var settings = new InstanceFirmwareSettings();
        if (document["properties"] is JsonObject properties)
        {
            if (properties.Count > 512) throw new InvalidDataException("实例固件设置属性过多");
            foreach (var pair in properties)
            {
                if (!ValidKey(pair.Key)) throw new InvalidDataException("实例固件设置包含无效键");
                if (pair.Value is null || pair.Value.GetValueKind() == JsonValueKind.Null)
                {
                    settings.Properties[pair.Key] = null;
                    continue;
                }
                if (JsonRead.AsString(pair.Value) is not { } text)
                    throw new InvalidDataException("实例固件设置包含无效键值");
                if (!ValidValue(text)) throw new InvalidDataException("实例固件设置包含无效键值");
                settings.Properties[pair.Key] = text;
            }
        }
        if (document["clock"] is JsonObject clock)
        {
            var mode = JsonRead.AsString(clock["mode"]);
            if (mode is not ("host" or "offset" or "firmware")) throw new InvalidDataException("实例时钟模式无效");
            settings.ClockMode = mode;
            if (clock["offsetSeconds"] is not null)
            {
                var offset = JsonRead.AsInt(clock["offsetSeconds"], int.MinValue);
                if (Math.Abs((long)offset) > MaximumOffsetSeconds) throw new InvalidDataException("实例时钟偏移超限");
                settings.ClockOffsetSeconds = offset;
            }
        }
        if (document["battery"] is JsonObject battery)
        {
            if (battery["level"] is not null)
            {
                var level = JsonRead.AsInt(battery["level"], -1);
                if (level is < 0 or > 100) throw new InvalidDataException("电池电量设定无效");
                var value = settings.Battery ?? new BatterySettings();
                value.Level = level;
                settings.Battery = value;
            }
            if (battery["sampleRaw"] is not null)
            {
                var raw = JsonRead.AsInt(battery["sampleRaw"], -1);
                if (raw is < 0 or > MaximumSampleRaw) throw new InvalidDataException("电池采样设定无效");
                var value = settings.Battery ?? new BatterySettings();
                value.SampleRaw = raw;
                settings.Battery = value;
            }
            var charger = JsonRead.AsString(battery["charger"]);
            var gauge = JsonRead.AsString(battery["gauge"]);
            if (charger is not null && !Chargers.Contains(charger)) throw new InvalidDataException("充电器状态无效");
            if (gauge is not null and not ("auto" or "hp4560" or "hp4570")) throw new InvalidDataException("电量计型号无效");
            if (charger is not null || gauge is not null)
            {
                var value = settings.Battery ?? new BatterySettings();
                value.Charger = charger ?? "auto";
                value.Gauge = gauge ?? "auto";
                settings.Battery = value;
            }
        }
        if (document["health"] is JsonObject health)
        {
            var value = new HealthSettings();
            var walk = health["walkSteps"] is not null ? health["walkSteps"] : health["steps"];
            if (walk is not null)
            {
                var steps = JsonRead.AsInt(walk, -1);
                if (steps is < 0 or > MaximumSteps) throw new InvalidDataException("开机步数无效");
                value.WalkSteps = steps;
            }
            if (health["cadence"] is not null)
            {
                var cadence = JsonRead.AsInt(health["cadence"], -1);
                if (cadence is < 40 or > 200) throw new InvalidDataException("步频设定无效");
                value.Cadence = cadence;
            }
            if (health["heartRate"] is not null)
            {
                var heartRate = JsonRead.AsInt(health["heartRate"], -1);
                if (heartRate is < 0 or > MaximumHeartRate) throw new InvalidDataException("心率设定无效");
                value.HeartRate = heartRate;
            }
            if (health["worn"] is not null)
            {
                if (health["worn"]!.GetValueKind() != JsonValueKind.False && health["worn"]!.GetValueKind() != JsonValueKind.True)
                    throw new InvalidDataException("佩戴设定无效");
                value.Worn = JsonRead.AsBool(health["worn"]);
            }
            if (!value.IsEmpty) settings.Health = value;
        }
        return settings;
    }
}

/// <summary>The instance-settings CLI receipt (describe()).</summary>
public sealed class SettingsReceipt
{
    public required InstanceFirmwareSettings Settings { get; set; }
    public IReadOnlyDictionary<string, string>? Observed { get; set; }
    public int? ObservedAtUnix { get; set; }
    public IReadOnlyList<(string ModelPrefix, bool Level, bool Charger)> BatteryInputs { get; set; } = [];
    public IReadOnlyList<(string ModelPrefix, bool Steps, bool HeartRate)> HealthInputs { get; set; } = [];
    public string? SettingsError { get; set; }

    public (bool Level, bool Charger)? BatteryInputsFor(string? model) =>
        model is null ? null
            : BatteryInputs.FirstOrDefault(inputs => model.StartsWith(inputs.ModelPrefix, StringComparison.Ordinal))
            is var hit && hit.ModelPrefix is null ? null : (hit.Level, hit.Charger);

    public (bool Steps, bool HeartRate)? HealthInputsFor(string? model) =>
        model is null ? null
            : HealthInputs.FirstOrDefault(inputs => model.StartsWith(inputs.ModelPrefix, StringComparison.Ordinal))
            is var hit && hit.ModelPrefix is null ? null : (hit.Steps, hit.HeartRate);

    public static SettingsReceipt Decode(JsonNode node)
    {
        if (node is not JsonObject body) throw new InvalidDataException("实例固件设置回执不是对象");
        if (body["settings"] is not JsonObject settingsNode) throw new InvalidDataException("实例固件设置回执缺少 settings");
        var receipt = new SettingsReceipt { Settings = InstanceFirmwareSettings.Decode(settingsNode) };
        if (body["observed"] is JsonObject observed && observed["properties"] is JsonObject properties)
        {
            if (properties.Count > 4_096) throw new InvalidDataException("观测属性过多");
            receipt.Observed = properties.Where(pair => JsonRead.AsString(pair.Value) is not null)
                .ToDictionary(pair => pair.Key, pair => JsonRead.AsString(pair.Value)!);
            if (observed["recordedAtUnix"] is not null) receipt.ObservedAtUnix = JsonRead.AsInt(observed["recordedAtUnix"], -1);
        }
        if (body["batteryInputs"] is JsonArray battery)
            receipt.BatteryInputs = battery.OfType<JsonObject>()
                .Select(entry => (JsonRead.AsString(entry["modelPrefix"]) ?? "",
                    JsonRead.AsBool(entry["level"]), JsonRead.AsBool(entry["charger"]))).ToList();
        if (body["healthInputs"] is JsonArray health)
            receipt.HealthInputs = health.OfType<JsonObject>()
                .Select(entry => (JsonRead.AsString(entry["modelPrefix"]) ?? "",
                    JsonRead.AsBool(entry["steps"]), JsonRead.AsBool(entry["heartRate"]))).ToList();
        receipt.SettingsError = JsonRead.AsString(body["settingsError"]);
        return receipt;
    }
}

/// <summary>One live-get / live-set reply: the support matrix, the notes and the
/// core state after the last change (port of SimulatorLiveState).</summary>
public sealed class LiveState
{
    private readonly Dictionary<string, JsonNode?> _state;
    public IReadOnlyDictionary<string, bool> Supported { get; }
    public IReadOnlyDictionary<string, string> Notes { get; }

    private LiveState(Dictionary<string, bool> supported, Dictionary<string, string> notes, Dictionary<string, JsonNode?> state)
    {
        Supported = supported;
        Notes = notes;
        _state = state;
    }

    public static LiveState? Parse(JsonNode? reply)
    {
        if (reply?["supported"] is not JsonObject supported || reply["state"] is not JsonObject state) return null;
        var notes = reply["notes"] is JsonObject notesNode
            ? notesNode.Where(pair => JsonRead.AsString(pair.Value) is not null)
                .ToDictionary(pair => pair.Key, pair => JsonRead.AsString(pair.Value)!)
            : new Dictionary<string, string>();
        // Swift's compactMapValues(\.bool) keeps only real booleans; a string or
        // number in the matrix is not a support flag.
        return new LiveState(supported.Where(pair => pair.Value is not null
                && (pair.Value.GetValueKind() == JsonValueKind.True || pair.Value.GetValueKind() == JsonValueKind.False))
            .ToDictionary(pair => pair.Key, pair => JsonRead.AsBool(pair.Value)),
            notes, state.ToDictionary(pair => pair.Key, pair => (JsonNode?)pair.Value));
    }

    public bool Supports(string key) => Supported.TryGetValue(key, out var value) && value;
    public string Note(string key) => Notes.TryGetValue(key, out var note) ? note : "";

    public int? Int(string key) => _state.TryGetValue(key, out var node) && JsonRead.TryNumber(node, out var number)
        && Math.Round(number) == number ? (int)number : null;
    public double? Number(string key) => _state.TryGetValue(key, out var node) && JsonRead.TryNumber(node, out var number)
        ? number : null;
    public bool? Bool(string key)
    {
        if (!_state.TryGetValue(key, out var node) || node is null) return null;
        return node.GetValueKind() switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
    }
    public string? Charger => _state.TryGetValue("charger", out var node) ? JsonRead.AsString(node) : null;
    public int StepsRemaining => Math.Max(0, (Int("walkSteps") ?? 0) - (Int("walked") ?? 0));
}
