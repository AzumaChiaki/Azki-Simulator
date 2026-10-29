using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;

namespace AzkiSimulator.Linux.ViewModels;

/// <summary>
/// Port of SimulatorDeviceDataView: every data input of the instance in one
/// table — the wearer (on the wrist, pulse, cadence, walking) and the board
/// (battery, charger, temperature). While the device runs, "应用" changes what
/// the emulated hardware senses at once (live-set) and keeps the value for the
/// next boot; when it is stopped, the value is saved for the next boot. Rows
/// appear only for inputs that really drive this preset's firmware.
/// All mutations run on the UI thread; the bridge and CLI calls do not.
/// </summary>
public sealed class DeviceDataViewModel : ObservableObject
{
    private readonly InstanceRecord _record;
    private readonly InspectionRunner? _runner;
    private readonly Func<BridgeClient?> _client;
    private readonly Func<bool> _isRunning;
    private bool _busy;
    private bool _adoptedFromSettings;
    private bool _adoptedFromLive;
    private InstanceFirmwareSettings? _settings;
    private SettingsReceipt? _receipt;
    private LiveState? _live;
    private string? _liveUnavailable;

    private bool _worn = true;
    private string _heartRateText = "";
    private string _cadenceText = "";
    private string _walkText = "100";
    private string _bootWalkText = "";
    private string _levelText = "";
    private string _charger = "unplugged";
    private string _temperatureText = "";

    public DeviceDataViewModel(InstanceRecord record, InspectionRunner? runner,
        Func<BridgeClient?> client, Func<bool> isRunning)
    {
        _record = record;
        _runner = runner;
        _client = client;
        _isRunning = isRunning;
        ApplyWornCommand = new AsyncRelayCommand(ApplyWornAsync, () => CanApply && true);
        ApplyHeartRateCommand = new AsyncRelayCommand(ApplyHeartRateAsync, () => CanApply && ParseInt(HeartRateText) is { } v && InRange(v, InstanceFirmwareSettings.HeartRateRange));
        ApplyCadenceCommand = new AsyncRelayCommand(ApplyCadenceAsync, () => CanApply && ParseInt(CadenceText) is { } v && InRange(v, InstanceFirmwareSettings.CadenceRange));
        ApplyWalkCommand = new AsyncRelayCommand(ApplyWalkAsync, () => CanApply && _isRunning() && _live?.Supports("steps") == true
            && ParseInt(WalkText) is { } v && InRange(v, InstanceFirmwareSettings.AddStepsRange));
        ApplyBootWalkCommand = new AsyncRelayCommand(ApplyBootWalkAsync, () => CanApply
            && ParseInt(BootWalkText) is { } v && v >= 0 && v <= InstanceFirmwareSettings.MaximumSteps);
        ApplyLevelCommand = new AsyncRelayCommand(ApplyLevelAsync, () => CanApply
            && ParseInt(LevelText) is { } v && InRange(v, InstanceFirmwareSettings.BatteryLevelRange));
        ApplyChargerCommand = new AsyncRelayCommand(ApplyChargerAsync, () => CanApply && _charger != "auto");
        ApplyTemperatureCommand = new AsyncRelayCommand(ApplyTemperatureAsync, () => CanApply && _isRunning()
            && ParseDouble(TemperatureText) is { } value && value >= InstanceFirmwareSettings.TemperatureRange.Min
            && value <= InstanceFirmwareSettings.TemperatureRange.Max);
    }

    public AsyncRelayCommand ApplyWornCommand { get; }
    public AsyncRelayCommand ApplyHeartRateCommand { get; }
    public AsyncRelayCommand ApplyCadenceCommand { get; }
    public AsyncRelayCommand ApplyWalkCommand { get; }
    public AsyncRelayCommand ApplyBootWalkCommand { get; }
    public AsyncRelayCommand ApplyLevelCommand { get; }
    public AsyncRelayCommand ApplyChargerCommand { get; }
    public AsyncRelayCommand ApplyTemperatureCommand { get; }

    private bool CanApply => _settings is not null && !_busy;

    public bool Worn { get => _worn; set { if (Set(ref _worn, value)) RaiseView(); } }
    public int WornIndex { get => _worn ? 0 : 1; set { _worn = value == 0; RaiseView(); } }
    public int ChargerIndex
    {
        get => _charger switch { "charging" => 1, "full" => 2, _ => 0 };
        set { _charger = value switch { 1 => "charging", 2 => "full", _ => "unplugged" }; RaiseView(); }
    }
    public string HeartRateText { get => _heartRateText; set => Set(ref _heartRateText, value); }
    public string CadenceText { get => _cadenceText; set => Set(ref _cadenceText, value); }
    public string WalkText { get => _walkText; set => Set(ref _walkText, value); }
    public string BootWalkText { get => _bootWalkText; set => Set(ref _bootWalkText, value); }
    public string LevelText { get => _levelText; set => Set(ref _levelText, value); }
    public string Charger { get => _charger; set { if (Set(ref _charger, value)) SetChargerFromEdit(); } }
    public string TemperatureText { get => _temperatureText; set => Set(ref _temperatureText, value); }

    public bool MessageIsError { get; private set; }
    public bool HasMessage => _message.Length > 0;
    public bool HasError => MessageIsError && _message.Length > 0;
    public bool HasNote => !MessageIsError && _message.Length > 0;
    private string _message = "";
    public string Message { get => _message; private set => Set(ref _message, value); }

    private string _wornCurrent = "";
    private string _heartRateCurrent = "";
    private string _cadenceCurrent = "";
    private string _walkCurrent = "";
    private string _bootWalkCurrent = "";
    private string _levelCurrent = "";
    private string _chargerCurrent = "";
    private string _temperatureCurrent = "";
    public string WornCurrent { get => _wornCurrent; private set => Set(ref _wornCurrent, value); }
    public string HeartRateCurrent { get => _heartRateCurrent; private set => Set(ref _heartRateCurrent, value); }
    public string CadenceCurrent { get => _cadenceCurrent; private set => Set(ref _cadenceCurrent, value); }
    public string WalkCurrent { get => _walkCurrent; private set => Set(ref _walkCurrent, value); }
    public string BootWalkCurrent { get => _bootWalkCurrent; private set => Set(ref _bootWalkCurrent, value); }
    public string LevelCurrent { get => _levelCurrent; private set => Set(ref _levelCurrent, value); }
    public string ChargerCurrent { get => _chargerCurrent; private set => Set(ref _chargerCurrent, value); }
    public string TemperatureCurrent { get => _temperatureCurrent; private set => Set(ref _temperatureCurrent, value); }

    private bool ShowsWearRow => (_health?.HeartRate ?? false) || _live?.Supports("heartRate") == true;
    private bool ShowsStepsRow => (_health?.Steps ?? false) || _live?.Supports("steps") == true;
    private bool ShowsLevelRow => (_battery?.Level ?? false) || _live?.Supports("batteryLevel") == true;
    private bool ShowsChargerRow => (_battery?.Charger ?? false) || _live?.Supports("charger") == true;
    private bool ShowsTemperatureRow => _live?.Supports("temperature") == true;
    private bool HasRows => ShowsWearRow || ShowsStepsRow || ShowsLevelRow || ShowsChargerRow || ShowsTemperatureRow;
    public bool ShowsWear => HasRows && ShowsWearRow;
    public bool ShowsSteps => HasRows && ShowsStepsRow;
    public bool ShowsBattery => HasRows && (ShowsLevelRow || ShowsChargerRow || ShowsTemperatureRow);
    public bool ShowsLevel => HasRows && ShowsLevelRow;
    public bool ShowsCharger => HasRows && ShowsChargerRow;
    public bool ShowsTemperature => HasRows && ShowsTemperatureRow;
    public bool ShowsNoRowsNote => _settings is not null && !HasRows;
    public string NoRowsNote => "此型号的传感器与电池输入尚未验证能驱动固件，因此不提供修改。";
    public bool ShowsLiveUnavailable => _liveUnavailable is not null && _isRunning();
    public string LiveUnavailableText => _liveUnavailable ?? "";
    public string Footer => BuildFooter();

    private (bool Steps, bool HeartRate)? _health => _receipt?.HealthInputsFor(_record.Model);
    private (bool Level, bool Charger)? _battery => _receipt?.BatteryInputsFor(_record.Model);

    public void Reset()
    {
        _busy = false;
        _adoptedFromSettings = false;
        _adoptedFromLive = false;
        _settings = null;
        _receipt = null;
        _live = null;
        _liveUnavailable = null;
        Message = "";
        MessageIsError = false;
        Raise(nameof(HasMessage));
        Raise(nameof(HasError));
        Raise(nameof(HasNote));
        RaiseView();
    }

    /// <summary>Reads the persisted settings once per session start; a failure is a
    /// message, never an exception into the caller.</summary>
    public async Task LoadAsync()
    {
        if (_runner is not null)
        {
            try
            {
                var receipt = await ReadReceiptAsync();
                AdoptSettings(receipt);
                if (receipt.SettingsError is { } problem) Report(problem, true);
            }
            catch (Exception error) when (error is IOException or InvalidDataException or BridgeRemoteError
                or TimeoutException or UnauthorizedAccessException)
            {
                Report(error.Message, true);
                _settings ??= new InstanceFirmwareSettings();
            }
            RaiseView();
        }
        await PollOnceAsync();
    }

    private async Task<SettingsReceipt> ReadReceiptAsync()
    {
        var body = await _runner!.RunCheckedAsync("instance-settings",
            ["--instance-id", _record.Id]);
        return SettingsReceipt.Decode(body);
    }

    private void AdoptSettings(SettingsReceipt receipt)
    {
        _receipt = receipt;
        var first = !_adoptedFromSettings;
        _settings = receipt.Settings;
        if (!first) return;
        _adoptedFromSettings = true;
        var health = receipt.Settings.Health;
        _worn = health?.Worn ?? true;
        _heartRateText = (health?.HeartRate ?? InstanceFirmwareSettings.RestingHeartRate).ToString();
        _cadenceText = (health?.Cadence ?? 110).ToString();
        _bootWalkText = (health?.WalkSteps ?? 0).ToString();
        _levelText = receipt.Settings.Battery?.Level?.ToString() ?? "";
        var charger = receipt.Settings.Battery?.Charger;
        _charger = charger is "charging" or "full" or "unplugged" ? charger : "unplugged";
    }

    private void Receive(LiveState? state)
    {
        if (state is null) return;
        var first = !_adoptedFromLive;
        _live = state;
        _liveUnavailable = null;
        if (!first) return;
        _adoptedFromLive = true;
        if (state.Bool("worn") is { } worn) _worn = worn;
        if (state.Int("heartRate") is > 0 and var rate) _heartRateText = rate.ToString();
        if (state.Int("cadence") is { } cadence) _cadenceText = cadence.ToString();
        var liveCharger = state.Charger;
        if (liveCharger is "charging" or "full" or "unplugged") _charger = liveCharger;
        if (state.Number("temperatureC") is { } temperature) _temperatureText = temperature.ToString("0.0");
    }

    /// <summary>One live-get attempt; the caller keeps the loop. Errors while no state
    /// was ever read explain why the panel is read-only for this run.</summary>
    public async Task<bool> PollOnceAsync()
    {
        var client = _client();
        if (!_isRunning() || client is null) return false;
        try
        {
            var reply = await Task.Run(() => client.Command("live-get"));
            Receive(LiveState.Parse(reply));
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError)
        {
            if (_live is null)
            {
                var text = error.Message;
                _liveUnavailable = text.Contains("[unsupported") || text.Contains("不受支持")
                    ? "当前运行的核心不提供实时输入，修改将在下次开机生效。"
                    : "暂时读不到实时输入：" + text;
            }
        }
        RaiseView();
        return _live is not null;
    }

    public bool WalkingNow => _live is not null && _live.StepsRemaining > 0;

    private async Task ApplyWornAsync() => await ApplyAsync(
        _live?.Supports("heartRate") == true ? new JsonObject { ["worn"] = _worn } : null,
        settings => { settings.HealthNode().Worn = _worn ? null : false; });

    private async Task ApplyHeartRateAsync()
    {
        if (ParseInt(_heartRateText) is not { } rate || !InRange(rate, InstanceFirmwareSettings.HeartRateRange)) return;
        await ApplyAsync(_live?.Supports("heartRate") == true ? new JsonObject { ["heartRate"] = rate } : null,
            settings => settings.HealthNode().HeartRate = rate);
    }

    private async Task ApplyCadenceAsync()
    {
        if (ParseInt(_cadenceText) is not { } cadence || !InRange(cadence, InstanceFirmwareSettings.CadenceRange)) return;
        await ApplyAsync(_live?.Supports("steps") == true ? new JsonObject { ["cadence"] = cadence } : null,
            settings => settings.HealthNode().Cadence = cadence);
    }

    private async Task ApplyWalkAsync()
    {
        if (ParseInt(_walkText) is not { } steps || !InRange(steps, InstanceFirmwareSettings.AddStepsRange)) return;
        await ApplyAsync(new JsonObject { ["addSteps"] = steps }, null);
    }

    private async Task ApplyBootWalkAsync()
    {
        if (ParseInt(_bootWalkText) is not { } steps || steps < 0 || steps > InstanceFirmwareSettings.MaximumSteps) return;
        await ApplyAsync(null, settings => settings.HealthNode().WalkSteps = steps == 0 ? null : steps);
    }

    private async Task ApplyLevelAsync()
    {
        if (ParseInt(_levelText) is not { } level || !InRange(level, InstanceFirmwareSettings.BatteryLevelRange)) return;
        await ApplyAsync(_live?.Supports("batteryLevel") == true ? new JsonObject { ["batteryLevel"] = level } : null,
            settings => settings.BatteryNode().Level = level);
    }

    private async Task ApplyChargerAsync()
    {
        if (_charger is not ("unplugged" or "charging" or "full")) return;
        await ApplyAsync(_live?.Supports("charger") == true ? new JsonObject { ["charger"] = _charger } : null,
            settings => settings.BatteryNode().Charger = _charger);
    }

    private async Task ApplyTemperatureAsync()
    {
        if (ParseDouble(_temperatureText) is not { } temperature
            || temperature < InstanceFirmwareSettings.TemperatureRange.Min || temperature > InstanceFirmwareSettings.TemperatureRange.Max)
            return;
        await ApplyAsync(new JsonObject { ["temperatureC"] = temperature }, null);
    }

    /// <summary>Live change first (only while running), then the next-boot value —
    /// the Swift apply ordering, including the uncertain-outcome wording.</summary>
    private async Task ApplyAsync(JsonObject? live, Action<InstanceFirmwareSettings>? boot)
    {
        if (_busy || _settings is null) return;
        _busy = true;
        RaiseView();
        var done = new List<string>();
        try
        {
            var client = _client();
            if (live is not null && _isRunning() && _live is not null && client is not null)
            {
                var reply = await Task.Run(() => client.Command("live-set", live));
                Receive(LiveState.Parse(reply));
                done.Add("已立即生效");
            }
            if (boot is not null && _runner is not null)
            {
                var receipt = await ReadReceiptAsync(); // re-read so sheet edits are kept
                var next = receipt.Settings;
                boot(next);
                var saved = await _runner.RunCheckedAsync("instance-settings",
                    ["--instance-id", _record.Id, "--set", next.Encode().ToJsonString()]);
                AdoptSettings(SettingsReceipt.Decode(saved));
                done.Add("已保存为下次开机的值");
            }
            Report(string.Join("，", done), false);
        }
        catch (Exception error) when (error is IOException or InvalidDataException or BridgeRemoteError
            or TimeoutException or UnauthorizedAccessException)
        {
            Report(error.Message.Contains("[live_outcome_unknown]")
                ? "写入可能已生效但未得到确认，请看设备画面或等待刷新，不要重复提交。"
                : error.Message, true);
        }
        finally
        {
            _busy = false;
            RaiseView();
        }
    }

    private void Report(string text, bool isError)
    {
        Message = text;
        MessageIsError = isError;
        Raise(nameof(HasMessage));
        Raise(nameof(HasError));
        Raise(nameof(HasNote));
        Raise(nameof(MessageIsError));
    }

    private void SetChargerFromEdit() => RaiseView();

    private string BootWornText => _settings?.Health?.Worn == false ? "未佩戴（开机）" : "佩戴（开机）";
    private string BootHeartRateText => $"{_settings?.Health?.HeartRate ?? InstanceFirmwareSettings.RestingHeartRate} 次/分（开机）";

    private string ChargerTitle(string value) => value switch
    {
        "charging" => "充电中",
        "full" => "已充满",
        _ => "未接充电器",
    };

    private string BuildFooter()
    {
        var lines = new List<string>
        {
            _isRunning()
                ? "设备运行中：“应用”立即改变模拟硬件的输入，并保存为下次开机的值；“保存”只作用于下次开机。"
                : "设备未运行：修改保存为下次开机的值；运行后可在此立即修改。",
            "步数、心率、电量都由固件自己的算法从模拟的传感器和电池数据算出：心率可能相差 1–3 次/分，未佩戴时固件提示佩戴；运行中改电量只改变电池电压，固件电量计逐步追随。",
        };
        if (ShowsStepsRow) lines.Add("开机后步行在固件计步服务就绪（约 75 秒）后开始，起步的几步用于识别步行。");
        if (ShowsLevelRow && _record.Model.StartsWith("miwear.watch.q66", StringComparison.Ordinal))
            lines.Add("手环 11 冷启动只按电池电压估算电量，电压 7 mV 一档，少数设定值会显示相差 1%。");
        return string.Join("\n", lines);
    }

    private void RaiseView()
    {
        UpdateCurrents();
        foreach (var name in new[]
        {
            nameof(ShowsWear), nameof(ShowsSteps), nameof(ShowsBattery), nameof(ShowsLevel), nameof(ShowsCharger),
            nameof(ShowsTemperature), nameof(ShowsNoRowsNote), nameof(ShowsLiveUnavailable), nameof(LiveUnavailableText),
            nameof(WornCurrent), nameof(HeartRateCurrent), nameof(CadenceCurrent), nameof(WalkCurrent),
            nameof(BootWalkCurrent), nameof(LevelCurrent), nameof(ChargerCurrent), nameof(TemperatureCurrent),
            nameof(Footer), nameof(Worn), nameof(WornIndex), nameof(Charger), nameof(ChargerIndex),
        }) Raise(name);
        foreach (var command in new[]
        {
            ApplyWornCommand, ApplyHeartRateCommand, ApplyCadenceCommand, ApplyWalkCommand,
            ApplyBootWalkCommand, ApplyLevelCommand, ApplyChargerCommand, ApplyTemperatureCommand,
        }) command.RaiseCanExecute();
    }

    /// <summary>The "当前" column: the live board state while running, the next-boot
    /// setting otherwise — the same precedence as the Swift view.</summary>
    private void UpdateCurrents()
    {
        WornCurrent = _live?.Bool("worn") is { } wornLive ? (wornLive ? "佩戴中" : "未佩戴") : BootWornText;
        HeartRateCurrent = _live?.Int("heartRate") is { } liveRate ? $"{liveRate} 次/分" : BootHeartRateText;
        CadenceCurrent = _live?.Int("cadence") is { } liveCadence ? $"{liveCadence} 步/分"
            : _settings?.Health?.Cadence is { } bootCadence ? $"{bootCadence} 步/分（开机）" : "110 步/分";
        WalkCurrent = _live is { } live && live.Supports("steps")
            ? $"已走 {live.Int("walked") ?? 0} 步" + (live.StepsRemaining > 0 ? $"，还剩 {live.StepsRemaining}" : "")
            : _isRunning() ? "—" : "未运行";
        BootWalkCurrent = _settings?.Health?.WalkSteps is { } walkSteps ? $"{walkSteps} 步" : "不走";
        LevelCurrent = _settings?.Battery?.Level is { } level ? $"开机 {level}%" : "固件默认";
        ChargerCurrent = _live?.Charger is { } liveCharger && liveCharger is not "auto"
            ? ChargerTitle(liveCharger)
            : ChargerTitle(_settings?.Battery?.Charger ?? "auto") + "（开机）";
        TemperatureCurrent = _live?.Number("temperatureC") is { } temperature ? $"{temperature:0.0} °C" : "—";
    }

    private static int? ParseInt(string text) =>
        int.TryParse(text.Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var value)
            ? value : null;

    private static double? ParseDouble(string text) =>
        double.TryParse(text.Trim(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var value)
        && double.IsFinite(value) ? value : null;

    private static bool InRange(int value, (int Min, int Max) range) => value >= range.Min && value <= range.Max;
}

internal static class InstanceFirmwareSettingsEdit
{
    /// <summary>Port of the Swift health(_:)/battery(_:) mutators: the block is
    /// created on demand because the saved document may not carry it yet.</summary>
    public static InstanceFirmwareSettings.HealthSettings HealthNode(this InstanceFirmwareSettings settings) =>
        settings.Health ??= new InstanceFirmwareSettings.HealthSettings();

    public static InstanceFirmwareSettings.BatterySettings BatteryNode(this InstanceFirmwareSettings settings) =>
        settings.Battery ??= new InstanceFirmwareSettings.BatterySettings();
}
