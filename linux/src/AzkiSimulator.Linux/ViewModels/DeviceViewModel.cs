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

/// <summary>One live instance session: supervisor lifecycle, RPC commands and the frame display.
/// Every bridge exchange is a blocking Unix socket round trip, so nothing on this class's
/// command path runs on the UI thread; state that views bind to is set back on the UI thread.</summary>
public sealed class DeviceViewModel : ObservableObject, IAsyncDisposable
{
    private readonly EngineLayout _layout;
    private readonly InstanceRecord _record;
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
        StartCommand = new AsyncRelayCommand(StartAsync, () => !Running);
        StopCommand = new AsyncRelayCommand(StopAsync, () => Running);
        RestartCommand = new AsyncRelayCommand(RestartAsync, () => Running);
        ScreenshotCommand = new AsyncRelayCommand(ScreenshotAsync, () => Running);
        ApplyAdcCommand = new AsyncRelayCommand(ApplyAdcAsync, () => Running && _adcDraft.HasChanges);
        RefreshAdcCommand = new AsyncRelayCommand(RefreshAdcAsync, () => Running);
        CrownClickCommand = new AsyncRelayCommand(() => ButtonAsync("crown", 1), () => Running && SupportsButtons);
        LowerClickCommand = new AsyncRelayCommand(() => ButtonAsync("lower", 1), () => Running && SupportsButtons);
        LowerDoubleClickCommand = new AsyncRelayCommand(() => ButtonAsync("lower", 2), () => Running && SupportsButtons);
        for (var channel = 0; channel < 8; channel++)
            AdcChannels.Add(new PmuAdcChannelViewModel
            {
                Channel = channel,
                Title = PmuAdc.Title(channel, _record.SelectedProfileId),
                Purpose = PmuAdc.Purpose(channel, _record.SelectedProfileId),
            });
    }

    public InstanceRecord Record => _record;
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
    public AsyncRelayCommand ApplyAdcCommand { get; }
    public AsyncRelayCommand RefreshAdcCommand { get; }
    public AsyncRelayCommand CrownClickCommand { get; }
    public AsyncRelayCommand LowerClickCommand { get; }
    public AsyncRelayCommand LowerDoubleClickCommand { get; }

    public event Action<WriteableBitmap>? FrameUpdated;

    public async Task StartAsync()
    {
        if (Running) return;
        var profileId = _record.SelectedProfileId
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
            var stagedName = Guid.NewGuid() + ".bin";
            // Copy, hashing and the blocking install exchange all run off the UI thread,
            // and the hash streams: a watchface pack is tens of megabytes and must not be
            // buffered whole.
            await Task.Run(() =>
            {
                var staging = Path.Combine(supervisor.SessionDirectory, "incoming");
                Directory.CreateDirectory(staging);
                var staged = Path.Combine(staging, stagedName);
                File.Copy(packagePath, staged);
                using var handle = File.OpenRead(staged);
                var sha = Convert.ToHexString(SHA256.HashData(handle)).ToLowerInvariant();
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

    private async Task LogPumpAsync()
    {
        var cursor = new JsonObject();
        while (Running && _supervisor is not null)
        {
            List<string>? appended = null;
            bool reset = false;
            JsonObject? next = null;
            try
            {
                var supervisor = _supervisor;
                var response = await Task.Run(() => supervisor.Client.Command("logs",
                    new JsonObject { ["stream"] = true, ["name"] = "stderr.log", ["limit"] = 8192, ["cursor"] = cursor }));
                if (response is not null)
                {
                    reset = JsonRead.AsBool(response["reset"]);
                    var data = JsonRead.AsString(response["dataBase64"]);
                    if (!string.IsNullOrEmpty(data))
                        appended = Encoding.UTF8.GetString(Convert.FromBase64String(data))
                            .Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
                    if (response["nextOffset"] is not null)
                        next = new JsonObject
                        {
                            ["streamID"] = JsonRead.AsString(response["streamID"]),
                            ["offset"] = JsonRead.AsInt(response["nextOffset"]),
                        };
                }
            }
            catch (Exception error)
            {
                // A dropped poll is retried on the next pass; the cursor simply stays put.
                if (error is not IOException and not BridgeRemoteError) UiFault.Report(error);
            }
            if (reset || appended is not null || next is not null)
            {
                if (next is not null) cursor = next;
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
            await Task.Delay(500);
        }
    }

    private void RaiseLifecycleCommands()
    {
        StartCommand.RaiseCanExecute();
        StopCommand.RaiseCanExecute();
        RestartCommand.RaiseCanExecute();
        ScreenshotCommand.RaiseCanExecute();
        ApplyAdcCommand.RaiseCanExecute();
        RefreshAdcCommand.RaiseCanExecute();
        CrownClickCommand.RaiseCanExecute();
        LowerClickCommand.RaiseCanExecute();
        LowerDoubleClickCommand.RaiseCanExecute();
    }

    private async Task DisposeSessionAsync()
    {
        if (_frames is not null) await _frames.DisposeAsync();
        _frames = null;
        if (_supervisor is not null) await _supervisor.DisposeAsync();
        _supervisor = null;
        lock (_inputGate) { _inputPending.Clear(); }
        Frame = null;
    }

    public async ValueTask DisposeAsync() => await DisposeSessionAsync();
}
