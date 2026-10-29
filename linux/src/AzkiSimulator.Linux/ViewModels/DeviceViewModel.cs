using System.Collections.ObjectModel;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.Services;

namespace AzkiSimulator.Linux.ViewModels;

public sealed class PmuAdcChannelViewModel : ObservableObject
{
    private string _text = "";
    private string _status = "尚未读取采样状态";
    private string _count = "尚无读取记录";
    public required int Channel { get; init; }
    public required string Title { get; init; }
    public required string Purpose { get; init; }
    public string Text { get => _text; set { Set(ref _text, value); } }
    public string StatusText { get => _status; set => Set(ref _status, value); }
    public string CountText { get => _count; set => Set(ref _count, value); }
}

public sealed record FirmwareOption(string Id, string DisplayName);

/// <summary>One live instance session: supervisor lifecycle, RPC commands and the frame display.
/// Every bridge exchange is a blocking Unix socket round trip, so nothing on this class's
/// command path runs on the UI thread; state that views bind to is set back on the UI thread.</summary>
public sealed class DeviceViewModel : ObservableObject, IAsyncDisposable
{
    private readonly EngineLayout _layout;
    private InstanceRecord _record;
    private readonly InspectionRunner _runner;
    private readonly InstanceStore _store = new();
    private readonly RecordingModel _recording = new();
    private readonly List<(int X, int Y, bool Down)> _inputPending = new();
    private readonly object _inputGate = new();
    private Task? _inputPump;
    private SessionSupervisor? _supervisor;
    private DeviceFrameService? _frames;
    private PmuAdcDraft _adcDraft = new();

    public DeviceViewModel(EngineLayout layout, InstanceRecord record)
    {
        _layout = layout;
        _record = record;
        _runner = new InspectionRunner(layout);
        _selectedProfileId = record.SelectedProfileId;
        Data = new DeviceDataViewModel(record, _runner,
            () => _supervisor?.Client, () => Running);
        StartCommand = new AsyncRelayCommand(StartAsync, () => !Running);
        StopCommand = new AsyncRelayCommand(StopAsync, () => Running);
        RestartCommand = new AsyncRelayCommand(RestartAsync, () => Running);
        ScreenshotCommand = new AsyncRelayCommand(ScreenshotAsync, () => Running);
        RecordCommand = new AsyncRelayCommand(ToggleRecordingAsync, () => Running && !_recording.IsFinishing);
        ApplyAdcCommand = new AsyncRelayCommand(ApplyAdcAsync, () => Running && _adcDraft.HasChanges);
        RefreshAdcCommand = new AsyncRelayCommand(RefreshAdcAsync, () => Running);
        CrownClickCommand = new AsyncRelayCommand(() => ButtonAsync("crown", 1), () => Running && SupportsButtons);
        LowerClickCommand = new AsyncRelayCommand(() => ButtonAsync("lower", 1), () => Running && SupportsButtons);
        LowerDoubleClickCommand = new AsyncRelayCommand(() => ButtonAsync("lower", 2), () => Running && SupportsButtons);
        OpenSystemSettingsCommand = new AsyncRelayCommand(OpenSystemSettingsAsync);
        SwitchPendingCommand = new AsyncRelayCommand(SwitchPendingAsync, () => Running && _pendingProfileId is not null);
        CancelPendingCommand = new AsyncRelayCommand(CancelPendingAsync, () => _pendingProfileId is not null);        // The recorder samples off the UI thread; its state reaches the view only
        // through this marshalled event.
        _recording.Changed += () => Dispatcher.UIThread.Post(() =>
        {
            RecordCommand.RaiseCanExecute();
            Raise(nameof(RecordingLabel));
            StatusText = RecordingStatus();
        });
        for (var channel = 0; channel < 8; channel++)
            AdcChannels.Add(new PmuAdcChannelViewModel
            {
                Channel = channel,
                Title = PmuAdc.Title(channel, _record.SelectedProfileId),
                Purpose = PmuAdc.Purpose(channel, _record.SelectedProfileId),
            });
    }

    public InstanceRecord Record => _record;
    public ConnectionSettings Settings => ConnectionSettings.Shared;
    public DeviceDataViewModel Data { get; }
    public ObservableCollection<PmuAdcChannelViewModel> AdcChannels { get; } = new();
    public ObservableCollection<string> LogLines { get; } = new();

    private bool _running;
    public bool Running { get => _running; private set { if (Set(ref _running, value)) RaiseLifecycleCommands(); } }

    private bool _supportsButtons;
    public bool SupportsButtons { get => _supportsButtons; private set { if (Set(ref _supportsButtons, value)) RaiseLifecycleCommands(); } }

    private string _statusText = "未启动";
    public string StatusText { get => _statusText; internal set => Set(ref _statusText, value); }

    private WriteableBitmap? _frame;
    public WriteableBitmap? Frame { get => _frame; private set => Set(ref _frame, value); }

    public SessionManifest? Manifest { get; private set; }
    public int PixelWidth => Manifest?.Width ?? 212;
    public int PixelHeight => Manifest?.Height ?? 520;

    public AsyncRelayCommand StartCommand { get; }
    public AsyncRelayCommand StopCommand { get; }
    public AsyncRelayCommand RestartCommand { get; }
    public AsyncRelayCommand ScreenshotCommand { get; }
    public AsyncRelayCommand RecordCommand { get; }
    public AsyncRelayCommand ApplyAdcCommand { get; }
    public AsyncRelayCommand RefreshAdcCommand { get; }
    public AsyncRelayCommand CrownClickCommand { get; }
    public AsyncRelayCommand LowerClickCommand { get; }
    public AsyncRelayCommand LowerDoubleClickCommand { get; }
    public AsyncRelayCommand OpenSystemSettingsCommand { get; }

    public event Action<FirmwareSettingsViewModel>? FirmwareSettingsRequested;

    private Task OpenSystemSettingsAsync()
    {
        if (FirmwareSettingsRequested is not { } request) return Task.CompletedTask;
        var sheet = new FirmwareSettingsViewModel(_record, _runner, RestartAsync, () => Running);
        request(sheet);
        return Task.CompletedTask;
    }

    public event Action<WriteableBitmap>? FrameUpdated;

    public async Task StartAsync()
    {
        if (Running) return;
        var profileId = _selectedProfileId
            ?? throw new InvalidOperationException("请先为实例选择固件版本。");
        try
        {
            StatusText = "准备会话中…";
            // The start choreography spawns sandboxed processes and waits on a socket,
            // so it runs away from the UI thread and only reports back.
            var supervisor = await Task.Run(() => SessionSupervisor.StartAsync(_layout, _record.Id, profileId,
                _record.VirtualIdentity?.Serial, _record.VirtualIdentity?.Address, _record.SkipOobe ?? true,
                CancellationToken.None));
            _supervisor = supervisor;
            StatusText = "启动辅助进程…";
            var client = await Task.Run(() => supervisor.LaunchAsync());
            Manifest = client.Manifest;
            _frames = new DeviceFrameService(supervisor.SessionDirectory, client.Manifest);
            _frames.Updated += bitmap => { Frame = bitmap; FrameUpdated?.Invoke(bitmap); };
            _frames.Failed += error => StatusText = error.Message;
            _frames.Disconnected += error =>
            {
                StatusText = error.Message;
                Running = false;
            };
            _frames.Start();
            RaiseLifecycleCommands();
            Running = true;
            StatusText = "运行中";
            await PollStatusOnce();
            _ = LogPumpAsync();
            await Data.LoadAsync();
            _ = DataPollLoopAsync();
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or InvalidDataException
            or BridgeRemoteError or TimeoutException)
        {
            StatusText = error.Message;
            await DisposeSessionAsync();
        }
    }

    public async Task StopAsync()
    {
        if (_supervisor is null) { Running = false; return; }
        await Task.Run(() =>
        {
            try { _supervisor.Client.Command("stop"); } catch (Exception) { /* teardown continues */ }
        });
        await DisposeSessionAsync();
        Running = false;
        StatusText = "已停止";
    }

    public async Task RestartAsync()
    {
        await StopAsync();
        await StartAsync();
    }

    public async Task ScreenshotAsync()
    {
        var supervisor = _supervisor;
        if (supervisor is null) return;
        try
        {
            var result = await Task.Run(() => supervisor.Client.Command("screenshot"));
            var file = JsonRead.AsString(result?["file"]);
            if (file is null || file.Contains('/') || file.Contains('\\') || file is "." or "..")
            {
                StatusText = "截图没有返回会话内的文件名。";
                return;
            }
            // The helper names the file, but the host still resolves it inside the
            // session directory rather than trusting whatever string came back.
            var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(supervisor.SessionDirectory));
            var source = Path.GetFullPath(Path.Combine(root, file));
            if (!source.StartsWith(root + Path.DirectorySeparatorChar) || !File.Exists(source))
            {
                StatusText = "截图文件不在会话目录内。";
                return;
            }
            var pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
            var target = Path.Combine(pictures, "Azki Simulator", $"screenshot-{DateTime.Now:yyyyMMdd-HHmmss}.png");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            await Task.Run(() => File.Copy(source, target));
            StatusText = $"截图已保存：{target}";
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError or UnauthorizedAccessException)
        {
            StatusText = error.Message;
        }
    }

    public void Touch(int x, int y, bool down)
    {
        lock (_inputGate)
        {
            // Consecutive moves collapse into the newest position; a press or a release
            // is never dropped, or the guest would keep a finger held down forever.
            if (down && _inputPending.Count > 0 && _inputPending[^1].Down) _inputPending[^1] = (x, y, true);
            else _inputPending.Add((x, y, down));
            // Enqueue and wake-up happen in one step: the pump clears the same field under
            // the same lock, so an event can never land in a queue nobody is draining.
            _inputPump ??= Task.Run(InputPumpAsync);
        }
    }

    private async Task InputPumpAsync()
    {
        while (true)
        {
            (int X, int Y, bool Down) next;
            lock (_inputGate)
            {
                if (_inputPending.Count == 0)
                {
                    _inputPump = null;
                    return;
                }
                next = _inputPending[0];
                _inputPending.RemoveAt(0);
            }
            var supervisor = _supervisor;
            if (supervisor is null) continue;
            try
            {
                supervisor.Client.Input(next.X, next.Y, next.Down);
            }
            catch (Exception error) when (error is IOException or BridgeRemoteError)
            {
                var message = error.Message;
                Dispatcher.UIThread.Post(() => StatusText = message);
            }
        }
    }

    private async Task ButtonAsync(string button, int clicks)
    {
        var supervisor = _supervisor;
        if (supervisor is null) return;
        try
        {
            await Task.Run(() => supervisor.Client.Command("device-button",
                new JsonObject { ["button"] = button, ["clicks"] = clicks }));
            StatusText = $"按键 {button} ×{clicks} 已确认。";
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError)
        {
            StatusText = error.Message;
        }
    }

    public async Task RefreshAdcAsync()
    {
        var supervisor = _supervisor;
        if (supervisor is null) return;
        JsonNode? response;
        try
        {
            response = await Task.Run(() => supervisor.Client.Command("pmu-adc-get"));
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError)
        {
            StatusText = error.Message;
            RaiseLifecycleCommands();
            return;
        }
        _adcDraft.Receive(response);
        for (var channel = 0; channel < 8; channel++)
        {
            var row = AdcChannels[channel];
            if (_adcDraft.CanAdopt(channel, row.Text)) row.Text = _adcDraft.Text[channel] ?? "";
            row.StatusText = PmuAdc.ReadStatus(channel, response);
            row.CountText = PmuAdc.ReadCount(channel, response);
        }
        RaiseLifecycleCommands();
    }

    public async Task ApplyAdcAsync()
    {
        var values = _adcDraft.Values;
        if (values is null) { StatusText = "八个采样通道都必须是 0…65535 的整数。"; return; }
        var supervisor = _supervisor;
        if (supervisor is null) return;
        try
        {
            await Task.Run(() => supervisor.Client.Command("pmu-adc-set",
                new JsonObject { ["values"] = new JsonArray(values.Select(value => (JsonNode)value).ToArray()) }));
            StatusText = "PMU ADC 输入已下发。";
            await RefreshAdcAsync();
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError)
        {
            StatusText = error.Message;
        }
    }

    public void UpdateAdcDraft(int channel, string text) => _adcDraft.Text[channel] = text;

    private string? _selectedProfileId;
    private FirmwareOption? _selectedFirmware;
    private string? _pendingProfileId;

    public ObservableCollection<FirmwareOption> FirmwareOptions { get; } = new();
    public bool HasPendingFirmware => _pendingProfileId is not null;

    public FirmwareOption? SelectedFirmware
    {
        get => _selectedFirmware;
        set
        {
            if (value is null || !Set(ref _selectedFirmware, value)) return;
            if (!Running)
            {
                _ = SwitchFirmwareAsync(value.Id);
                return;
            }
            _pendingProfileId = value.Id == _selectedProfileId ? null : value.Id;
            Raise(nameof(HasPendingFirmware));
            SwitchPendingCommand.RaiseCanExecute();
            CancelPendingCommand.RaiseCanExecute();
        }
    }

    public AsyncRelayCommand SwitchPendingCommand { get; }
    public AsyncRelayCommand CancelPendingCommand { get; }

    /// <summary>Port of SimulatorFirmwareControls: switching while serving is held
    /// as a pending choice until the explicit restart, which stops the recording first.</summary>
    private async Task SwitchPendingAsync()
    {
        var pending = _pendingProfileId;
        if (pending is null) return;
        _pendingProfileId = null;
        Raise(nameof(HasPendingFirmware));
        await _recording.StopAsync();
        await StopAsync();
        await SwitchFirmwareAsync(pending);
        await StartAsync();
    }

    private async Task CancelPendingAsync()
    {
        _pendingProfileId = null;
        _selectedFirmware = FirmwareOptions.FirstOrDefault(option => option.Id == _selectedProfileId);
        Raise(nameof(HasPendingFirmware));
        Raise(nameof(SelectedFirmware));
        SwitchPendingCommand.RaiseCanExecute();
        CancelPendingCommand.RaiseCanExecute();
    }

    private async Task SwitchFirmwareAsync(string profileId)
    {
        try
        {
            await Task.Run(() =>
            {
                var records = _store.Load().ToList();
                var index = records.FindIndex(candidate => candidate.Id == _record.Id);
                if (index < 0) throw new InvalidOperationException("实例记录已不存在。");
                var updated = records[index] with { SelectedProfileId = profileId };
                records[index] = updated;
                _store.Save(records);
                _record = updated;
                _selectedProfileId = profileId;
            });
            StatusText = "已切换固件（下次开机生效）。";
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException
            or UnauthorizedAccessException)
        {
            StatusText = "切换固件失败：" + error.Message;
        }
        Raise(nameof(Record));
    }

    public async Task RefreshFirmwareOptionsAsync()
    {
        try
        {
            var catalog = await Task.Run(() => _runner.RunCheckedAsync("library-catalog", Array.Empty<string>()));
            var options = (catalog["items"] as JsonArray ?? new JsonArray()).OfType<JsonObject>()
                .Select(row => (Id: JsonRead.AsString(row["id"]), Model: JsonRead.AsString(row["model"]),
                    Name: JsonRead.AsString(row["displayName"]), Ready: JsonRead.AsBool(row["availability"]?["ready"])))
                .Where(item => item.Id is not null && item.Model == _record.Model && item.Ready)
                .Select(item => new FirmwareOption(item.Id!, item.Name ?? item.Id!))
                .ToList();
            FirmwareOptions.Clear();
            foreach (var option in options) FirmwareOptions.Add(option);
            _selectedFirmware = FirmwareOptions.FirstOrDefault(option => option.Id == _selectedProfileId);
            Raise(nameof(SelectedFirmware));
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException
            or InvalidDataException)
        {
            StatusText = error.Message;
        }
    }

    public string RecordingLabel => _recording.IsRecording ? "停止录屏" : "录屏";

    private string RecordingStatus()
    {
        if (_recording.IsRecording) return $"录屏中 {_recording.Elapsed:hh\\:mm\\:ss}";
        if (_recording.ErrorMessage is { } error) return error;
        if (_recording.SavedFile is { } saved) return $"录屏已保存：{saved}";
        return "录屏已结束";
    }

    private async Task ToggleRecordingAsync()
    {
        if (_recording.IsRecording)
        {
            await _recording.StopAsync();
            StatusText = RecordingStatus();
            RecordCommand.RaiseCanExecute();
            Raise(nameof(RecordingLabel));
            return;
        }
        // The source reads the frame service's latest decoded frame; a null means the
        // session ended, which finalizes the movie rather than splicing a new session.
        if (!_recording.Start(_record.Name, () => _frames?.Latest))
        {
            StatusText = _recording.ErrorMessage ?? "无法开始录屏";
            RecordCommand.RaiseCanExecute();
            return;
        }
        StatusText = RecordingStatus();
        RecordCommand.RaiseCanExecute();
        Raise(nameof(RecordingLabel));
    }

    public async Task InstallAsync(string? packagePath, string kind)
    {
        if (packagePath is null || _supervisor is not { } supervisor)
        {
            StatusText = "请选择要安装的快应用或表盘包。";
            return;
        }
        if (Manifest is null) return;
        try
        {
            var label = kind == "watchface" ? "表盘" : "快应用";
            var identifier = Guid.NewGuid();
            // Copy, hashing and the blocking install exchange all run off the UI thread, and
            // the staging helper streams the bytes while it hashes: a watchface pack is tens
            // of megabytes and must not be buffered whole.
            await Task.Run(() =>
            {
                var (stagedName, sha, _) = PackageStaging.Stage(supervisor.SessionDirectory, packagePath, identifier);
                var status = supervisor.Client.Command("status");
                supervisor.Client.Command("guest-install", new JsonObject
                {
                    ["fileName"] = stagedName, ["sha256"] = sha,
                    ["expected_session_id"] = Manifest.Id,
                    ["expected_generation"] = JsonRead.AsInt(status["generation"]),
                });
            });
            StatusText = $"{label}安装请求已发送，见操作日志。";
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError or UnauthorizedAccessException)
        {
            StatusText = error.Message;
        }
    }

    private async Task<JsonNode?> PollStatusOnce()
    {
        var supervisor = _supervisor;
        if (supervisor is null) return null;
        JsonNode? status;
        try
        {
            status = await Task.Run(() => supervisor.Client.Command("status"));
        }
        catch (Exception error) when (error is IOException or BridgeRemoteError)
        {
            StatusText = error.Message;
            return null;
        }
        if (status is null) return null;
        SupportsButtons = JsonRead.AsStringList(status["capabilities"]?["commands"]).Contains("device-button");
        var state = JsonRead.AsString(status["state"]);
        StatusText = state switch
        {
            "running" => "运行中",
            "crashed" => $"异常退出（{JsonRead.AsInt(status["lastExit"], -1)}）",
            _ => "已停止",
        };
        if (state == "crashed") Running = false;
        return status;
    }

    private async Task DataPollLoopAsync()
    {
        // Same cadence as the Swift task(id: liveContext) loop: 3 s, or 1 s while
        // the modeled wearer is still walking steps.
        while (Running && _supervisor is not null)
        {
            await Data.PollOnceAsync();
            try { await Task.Delay(Data.WalkingNow ? 1_000 : 3_000); }
            catch (Exception) { return; }
        }
    }

    private async Task LogPumpAsync()
    {
        // The reference stream begins with a null cursor: the helper reads an absent cursor
        // as "start at the tail", while an empty object is a cursor it refuses.
        JsonNode? cursor = null;
        while (Running && _supervisor is not null)
        {
            List<string>? appended = null;
            bool reset = false;
            var fault = (string?)null;
            try
            {
                var supervisor = _supervisor;
                var response = await Task.Run(() => supervisor.Client.Command("logs", LogStream.Request(cursor)));
                if (response is not null)
                {
                    reset = JsonRead.AsBool(response["reset"]);
                    var data = JsonRead.AsString(response["dataBase64"]);
                    if (!string.IsNullOrEmpty(data))
                        appended = Encoding.UTF8.GetString(Convert.FromBase64String(data))
                            .Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
                    var (next, error) = LogStream.Advance(response);
                    cursor = next;
                    fault = error;
                }
            }
            catch (Exception error)
            {
                // A dropped poll is retried on the next pass; the cursor simply stays put.
                if (error is not IOException and not BridgeRemoteError) UiFault.Report(error);
            }
            if (reset || appended is not null || cursor is not null)
            {
                var captured = appended;
                // The collection is bound to a list view, so it changes on the UI thread.
                Dispatcher.UIThread.Post(() =>
                {
                    if (reset) LogLines.Clear();
                    if (captured is null) return;
                    foreach (var line in captured)
                    {
                        LogLines.Add(line);
                        while (LogLines.Count > 2000) LogLines.RemoveAt(0);
                    }
                });
            }
            if (fault is { } message)
            {
                Dispatcher.UIThread.Post(() => StatusText = message);
                return;
            }
            await Task.Delay(250);
        }
    }

    private void RaiseLifecycleCommands()
    {
        StartCommand.RaiseCanExecute();
        StopCommand.RaiseCanExecute();
        RestartCommand.RaiseCanExecute();
        ScreenshotCommand.RaiseCanExecute();
        RecordCommand.RaiseCanExecute();
        SwitchPendingCommand.RaiseCanExecute();
        CancelPendingCommand.RaiseCanExecute();
        ApplyAdcCommand.RaiseCanExecute();
        RefreshAdcCommand.RaiseCanExecute();
        CrownClickCommand.RaiseCanExecute();
        LowerClickCommand.RaiseCanExecute();
        LowerDoubleClickCommand.RaiseCanExecute();
    }

    private async Task DisposeSessionAsync()
    {
        // Finalize an in-flight recording first: it samples the frame service, and
        // Swift's comment about stopping before teardown applies to the movie too.
        try { await _recording.DisposeAsync(); }
        catch (Exception error) when (error is IOException or InvalidOperationException)
        {
            StatusText = "录屏收尾失败：" + error.Message;
        }
        if (_frames is not null) await _frames.DisposeAsync();
        _frames = null;
        if (_supervisor is not null) await _supervisor.DisposeAsync();
        _supervisor = null;
        Data.Reset();
        lock (_inputGate) { _inputPending.Clear(); }
        Frame = null;
    }

    public async ValueTask DisposeAsync() => await DisposeSessionAsync();
}
