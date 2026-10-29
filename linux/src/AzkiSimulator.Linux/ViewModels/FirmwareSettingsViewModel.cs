using System.Collections.ObjectModel;
using AzkiSimulator.Linux.Core;

namespace AzkiSimulator.Linux.ViewModels;

/// <summary>One 系统 choice row (语言 / 时间格式 / 时区): keep, one of the values,
/// or restore the firmware default — the Choice enum of SimulatorFirmwareSettingsSheet
/// expressed as a ComboBox index. Index 0 is keep, the last is reset.</summary>
public sealed class FirmwareChoiceViewModel : ObservableObject
{
    private readonly InstanceFirmwareSettings _settings;
    private readonly Func<string> _keepLabel;
    private readonly (string Value, string Label)[] _options;
    private int _selectedIndex;

    public FirmwareChoiceViewModel(InstanceFirmwareSettings settings, string title, string key,
        (string Value, string Label)[] options, string? observedValue, Func<string> keepLabel)
    {
        _settings = settings;
        Title = title;
        Key = key;
        _keepLabel = keepLabel;
        var all = options.ToList();
        if (_settings.Properties.TryGetValue(key, out var current) && current is not null
            && !all.Any(option => option.Value == current))
            all.Insert(0, (current, current));
        _options = all.ToArray();
        ObservedValue = observedValue;
        Labels = new List<string> { KeepLabel() }.Concat(_options.Select(option => option.Label)).Append("恢复固件默认").ToList();
        _selectedIndex = Map();
    }

    public string Title { get; }
    public string Key { get; }
    public string? ObservedValue { get; }
    public IReadOnlyList<string> Labels { get; }

    private string KeepLabel() => _keepLabel();

    private int Map()
    {
        if (!_settings.Properties.TryGetValue(Key, out var value)) return 0;
        if (value is null) return Labels.Count - 1;
        var index = Array.FindIndex(_options, option => option.Value == value);
        return index < 0 ? 0 : index + 1;
    }

    public int SelectedIndex
    {
        get => _selectedIndex;
        set
        {
            if (!Set(ref _selectedIndex, value)) return;
            if (value == 0) _settings.Properties.Remove(Key);
            else if (value == Labels.Count - 1) _settings.Properties[Key] = null;
            else _settings.Properties[Key] = _options[value - 1].Value;
        }
    }
}

/// <summary>One persist.* property row: the firmware's last observed value plus
/// the three override states (absent / override / restore-default).</summary>
public sealed class FirmwarePropertyRowViewModel : ObservableObject
{
    private readonly InstanceFirmwareSettings _settings;
    private string _valueText = "";
    private int _mode; // 0 absent, 1 override, 2 reset

    public FirmwarePropertyRowViewModel(InstanceFirmwareSettings settings, string key, string? observedValue)
    {
        _settings = settings;
        Key = key;
        ObservedText = observedValue is null ? "固件当前：未设置" : $"固件当前：{observedValue}";
        EditCommand = new RelayCommand(() => { Mode = 1; ValueText = observedValue ?? ""; _settings.Properties[key] = ValueText; });
        UndoCommand = new RelayCommand(() => { Mode = 0; _settings.Properties.Remove(key); });
        ResetDefaultCommand = new RelayCommand(() => { Mode = 2; _settings.Properties[key] = null; });
        if (_settings.Properties.TryGetValue(key, out var current))
        {
            Mode = current is null ? 2 : 1;
            _valueText = current ?? "";
        }
    }

    public string Key { get; }
    public string ObservedText { get; }
    public RelayCommand EditCommand { get; }
    public RelayCommand UndoCommand { get; }
    public RelayCommand ResetDefaultCommand { get; }
    public bool IsAbsent => Mode == 0;
    public bool IsOverride => Mode == 1;
    public bool IsReset => Mode == 2;
    public bool ShowsResetButton => Mode != 2;

    private int Mode
    {
        get => _mode;
        set
        {
            _mode = value;
            foreach (var name in new[] { nameof(IsAbsent), nameof(IsOverride), nameof(IsReset), nameof(ShowsResetButton) })
                Raise(name);
            EditCommand.RaiseCanExecute();
            UndoCommand.RaiseCanExecute();
            ResetDefaultCommand.RaiseCanExecute();
        }
    }

    public string ValueText
    {
        get => _valueText;
        set
        {
            _valueText = value;
            if (Mode == 1) _settings.Properties[Key] = value;
            Raise();
        }
    }
}

/// <summary>Port of SimulatorFirmwareSettingsSheet: writes the system settings
/// database (persist.db) the firmware sees at the next boot, plus the boot clock.
/// Wearer/battery inputs live in the data panel, not here. The firmware keeps any
/// change made on the device itself unless the same key is overridden here.</summary>
public sealed class FirmwareSettingsViewModel : ObservableObject
{
    private readonly InstanceRecord _record;
    private readonly InspectionRunner _runner;
    private readonly Func<Task> _restart;
    private readonly Func<bool> _restartAvailable;
    private InstanceFirmwareSettings _settings = new();
    private Dictionary<string, string> _observed = new();
    private DateTimeOffset _customTime = DateTimeOffset.Now;
    private bool _loaded;
    private bool _saving;
    private string _filter = "";

    public FirmwareSettingsViewModel(InstanceRecord record, InspectionRunner runner, Func<Task> restart,
        Func<bool> restartAvailable)
    {
        _record = record;
        _runner = runner;
        _restart = restart;
        _restartAvailable = restartAvailable;
        SaveCommand = new AsyncRelayCommand(() => SaveAsync(false), () => _loaded && !_saving);
        SaveRestartCommand = new AsyncRelayCommand(() => SaveAsync(true), () => _loaded && !_saving && _restartAvailable());
        RetryCommand = new AsyncRelayCommand(LoadAsync, () => !_saving);
        AddCommand = new RelayCommand(AddProperty);
    }

    public string RecordName => _record.Name;
    public bool Loaded { get => _loaded; private set => Set(ref _loaded, value); }
    public bool ShowsBusy { get; private set; }
    public bool LoadFailed { get; private set; }
    public bool RestartAvailable => _restartAvailable();
    public AsyncRelayCommand SaveCommand { get; }
    public AsyncRelayCommand SaveRestartCommand { get; }
    public AsyncRelayCommand RetryCommand { get; }
    public RelayCommand AddCommand { get; }
    public ObservableCollection<FirmwareChoiceViewModel> Choices { get; } = new();
    public ObservableCollection<FirmwarePropertyRowViewModel> Rows { get; } = new();
    public bool Saved { get; private set; }

    /// <summary>Raised once after a successful save; the window that hosts the sheet
    /// closes itself on this, mirroring the Swift dismiss-then-restart order.</summary>
    public event Action? SavedRequested;

    private string _message = "";
    public string Message { get => _message; private set => Set(ref _message, value); }
    public bool HasMessage => Message.Length > 0;

    private string _newKey = "";
    private string _newValue = "";
    public string NewKey { get => _newKey; set => Set(ref _newKey, value); }
    public string NewValue { get => _newValue; set => Set(ref _newValue, value); }

    public string Filter
    {
        get => _filter;
        set { if (Set(ref _filter, value)) RebuildRows(); }
    }

    private string _observedAtText = "";
    public string ObservedAtText { get => _observedAtText; private set => Set(ref _observedAtText, value); }
    public bool ShowsObservedAt => ObservedAtText.Length > 0;

    public int ClockModeIndex
    {
        get => _settings.ClockMode switch { "offset" => 1, "firmware" => 2, _ => 0 };
        set
        {
            _settings.ClockMode = value switch { 1 => "offset", 2 => "firmware", _ => "host" };
            _settings.ClockOffsetSeconds = null;
            Raise();
            Raise(nameof(IsCustomClock));
        }
    }

    public bool IsCustomClock => _settings.ClockMode == "offset";
    public DateTimeOffset CustomTime { get => _customTime; set { _customTime = value; Raise(); } }

    public async Task LoadAsync()
    {
        Message = "";
        Raise(nameof(HasMessage));
        try
        {
            var body = await _runner.RunCheckedAsync("instance-settings", ["--instance-id", _record.Id]);
            Apply(SettingsReceipt.Decode(body));
            LoadFailed = false;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or BridgeRemoteError
            or TimeoutException or UnauthorizedAccessException)
        {
            Message = error.Message;
            LoadFailed = true;
        }
        Loaded = true;
        Raise(nameof(LoadFailed));
        Raise(nameof(HasMessage));
        Raise(nameof(Message));
        RaiseCommands();
    }

    private void Apply(SettingsReceipt receipt)
    {
        _settings = receipt.Settings;
        _observed = receipt.Observed?.ToDictionary(pair => pair.Key, pair => pair.Value) ?? [];
        if (_settings.ClockMode == "offset" && _settings.ClockOffsetSeconds is { } offset)
            _customTime = DateTimeOffset.Now.AddSeconds(offset);
        ObservedAtText = receipt.ObservedAtUnix is { } at
            ? $"固件当前值读取于 {DateTimeOffset.FromUnixTimeSeconds(at).LocalDateTime:yyyy-MM-dd HH:mm}" : "";
        Choices.Clear();
        Choices.Add(new FirmwareChoiceViewModel(_settings, "语言", InstanceFirmwareSettingKeys.Locale,
            InstanceFirmwareSettingKeys.Languages, Observed(InstanceFirmwareSettingKeys.Locale),
            () => ObservedLabel(InstanceFirmwareSettingKeys.Locale)));
        Choices.Add(new FirmwareChoiceViewModel(_settings, "时间格式", InstanceFirmwareSettingKeys.TimeFormat,
            [("false", "24 小时制"), ("true", "12 小时制")], Observed(InstanceFirmwareSettingKeys.TimeFormat),
            () => ObservedLabel(InstanceFirmwareSettingKeys.TimeFormat)));
        Choices.Add(new FirmwareChoiceViewModel(_settings, "时区", InstanceFirmwareSettingKeys.TimeZone,
            InstanceFirmwareSettingKeys.TimeZones.Select(zone => (zone, zone)).ToArray(),
            Observed(InstanceFirmwareSettingKeys.TimeZone),
            () => _settings.ClockMode == "host" ? $"跟随 Mac（{TimeZoneInfo.Local.Id}）" : "不修改"));
        RebuildRows();
        Raise(nameof(ClockModeIndex));
        Raise(nameof(IsCustomClock));
        Raise(nameof(CustomTime));
        Raise(nameof(ShowsObservedAt));
    }

    private string? Observed(string key) => _observed.TryGetValue(key, out var value) ? value : null;
    private string ObservedLabel(string key) => Observed(key) is { } value ? $"不修改（当前 {value}）" : "不修改";

    private void RebuildRows()
    {
        var needle = Filter.Trim().ToLowerInvariant();
        var keys = _observed.Keys.Concat(_settings.Properties.Keys)
            .Where(key => key != "persist.rtc_save")
            .Distinct()
            .Where(key => needle.Length == 0 || key.ToLowerInvariant().Contains(needle))
            .OrderBy(key => key, StringComparer.Ordinal);
        Rows.Clear();
        foreach (var key in keys) Rows.Add(new FirmwarePropertyRowViewModel(_settings, key, Observed(key)));
    }

    private void AddProperty()
    {
        var key = NewKey.Trim();
        if (!InstanceFirmwareSettings.ValidKey(key)
            || (NewValue.Length > 0 && !InstanceFirmwareSettings.ValidValue(NewValue)))
        {
            Report("属性名须以 persist. 开头，只含字母、数字、点、下划线或连字符；值不能含换行或制表符。");
            return;
        }
        _settings.Properties[key] = NewValue;
        NewKey = "";
        NewValue = "";
        Message = "";
        Raise(nameof(NewKey));
        Raise(nameof(NewValue));
        Raise(nameof(HasMessage));
        RebuildRows();
    }

    private async Task SaveAsync(bool thenRestart)
    {
        if (_saving) return;
        var bad = _settings.Properties.FirstOrDefault(pair => !InstanceFirmwareSettings.ValidKey(pair.Key)
            || (pair.Value is { } text && !InstanceFirmwareSettings.ValidValue(text)));
        if (bad.Key is not null)
        {
            Report($"属性 {bad.Key} 的值不能含换行或制表符，且不超过 1024 个字符。");
            return;
        }
        if (_settings.ClockMode == "offset")
        {
            var offset = (int)Math.Round((_customTime - DateTimeOffset.Now).TotalSeconds);
            if (Math.Abs((long)offset) > InstanceFirmwareSettings.MaximumOffsetSeconds)
            {
                Report("自定义时间超出可设置范围");
                return;
            }
            _settings.ClockOffsetSeconds = offset;
        }
        _saving = true;
        ShowsBusy = true;
        Raise(nameof(ShowsBusy));
        RaiseCommands();
        try
        {
            var body = await _runner.RunCheckedAsync("instance-settings",
                ["--instance-id", _record.Id, "--set", _settings.Encode().ToJsonString()]);
            Apply(SettingsReceipt.Decode(body));
            Message = "";
            Saved = true;
            SavedRequested?.Invoke();
            if (thenRestart) await _restart();
        }
        catch (Exception error) when (error is IOException or InvalidDataException or BridgeRemoteError
            or TimeoutException or UnauthorizedAccessException)
        {
            Report(error.Message);
        }
        finally
        {
            _saving = false;
            ShowsBusy = false;
            Raise(nameof(ShowsBusy));
            RaiseCommands();
        }
    }

    private void Report(string text)
    {
        Message = text;
        Raise(nameof(HasMessage));
        Raise(nameof(Message));
    }

    private void RaiseCommands()
    {
        SaveCommand.RaiseCanExecute();
        SaveRestartCommand.RaiseCanExecute();
        RetryCommand.RaiseCanExecute();
    }
}

/// <summary>The verified firmware keys and their option lists (InstanceFirmwareSettingKeys).</summary>
public static class InstanceFirmwareSettingKeys
{
    public const string Locale = "persist.locale";
    public const string TimeFormat = "persist.system_time_format"; // "true" = 12-hour
    public const string TimeZone = "persist.system_tz";            // IANA zone name

    public static readonly (string Value, string Label)[] Languages =
    [
        ("zh_CN", "简体中文"), ("zh_TW", "繁體中文（台灣）"), ("zh_HK", "繁體中文（香港）"),
        ("en_US", "English (US)"), ("en_GB", "English (UK)"), ("ja_JP", "日本語"), ("ko_KR", "한국어"),
        ("de_DE", "Deutsch"), ("fr_FR", "Français"), ("es_ES", "Español"), ("it_IT", "Italiano"),
        ("pt_BR", "Português (Brasil)"), ("ru_RU", "Русский"), ("th_TH", "ไทย"), ("vi_VN", "Tiếng Việt"),
    ];

    public static readonly string[] TimeZones =
    [
        "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Taipei", "Asia/Tokyo", "Asia/Seoul",
        "Asia/Singapore", "Asia/Bangkok", "Asia/Kolkata", "Asia/Dubai", "Europe/London", "Europe/Paris",
        "Europe/Berlin", "Europe/Moscow", "America/New_York", "America/Chicago", "America/Denver",
        "America/Los_Angeles", "America/Sao_Paulo", "Australia/Sydney", "Pacific/Auckland", "UTC",
    ];
}
