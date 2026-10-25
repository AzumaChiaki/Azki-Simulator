using System.Collections.ObjectModel;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia.Platform.Storage;
using AzkiSimulator.Linux.Core;

namespace AzkiSimulator.Linux.ViewModels;

public sealed record FirmwarePreset(string Model, string DisplayName, int Width, int Height);

/// <summary>Digest is carried with the row: deleting a firmware entry needs the full
/// OTA SHA-256, and asking the CLI again for it means a second sandboxed process.</summary>
public sealed record FirmwareItem(string Id, string DisplayName, string Model, string Version,
    string Digest, bool Ready, string Reason);

/// <summary>Instance library + firmware catalog, driving azki-simulator.py one-shot commands.
/// Every CLI exchange is a sandboxed subprocess, so the commands run off the UI thread.</summary>
public sealed class ManagerViewModel : ObservableObject
{
    private readonly EngineLayout? _layout;
    private readonly InspectionRunner? _inspection;
    private readonly InstanceStore _store = new();

    /// <summary>A tarball without its Engine tree is a readable status message, not an
    /// exception escaping an async command where no caller could report it.</summary>
    private InspectionRunner Engine => _inspection ?? throw new BridgeRemoteError(
        "未找到 Azki Simulator 引擎目录：请完整解包 tarball，或设置 AZKI_APP_ROOT。");

    public ManagerViewModel()
    {
        _layout = EngineLayout.TryResolve();
        if (_layout is { } layout) _inspection = new InspectionRunner(layout);
        NewInstanceCommand = new AsyncRelayCommand(NewInstanceAsync);
        DeleteInstanceCommand = new AsyncRelayCommand(DeleteInstanceAsync, () => SelectedInstance is not null);
        ResetInstanceCommand = new AsyncRelayCommand(ResetInstanceAsync, () => SelectedInstance is not null);
        OpenDeviceCommand = new AsyncRelayCommand(OpenDeviceAsync, () => SelectedInstance is not null);
        ImportFirmwareCommand = new AsyncRelayCommand(ImportFirmwareHintAsync);
        DeleteFirmwareCommand = new AsyncRelayCommand(DeleteFirmwareAsync, () => SelectedFirmware is not null);
        CleanupSessionsCommand = new AsyncRelayCommand(CleanupSessionsAsync);
        _ = RefreshAsync();
    }

    public ObservableCollection<InstanceRecord> Instances { get; } = new();
    public ObservableCollection<FirmwarePreset> Presets { get; } = new();
    public ObservableCollection<FirmwareItem> FirmwareItems { get; } = new();

    private InstanceRecord? _selectedInstance;
    public InstanceRecord? SelectedInstance { get => _selectedInstance; set { Set(ref _selectedInstance, value); RaiseCommands(); } }

    private FirmwareItem? _selectedFirmware;
    public FirmwareItem? SelectedFirmware { get => _selectedFirmware; set { Set(ref _selectedFirmware, value); RaiseCommands(); } }

    private string _statusText = "";
    public string StatusText { get => _statusText; internal set => Set(ref _statusText, value); }

    public AsyncRelayCommand OpenDeviceCommand { get; }
    public AsyncRelayCommand NewInstanceCommand { get; }
    public AsyncRelayCommand DeleteInstanceCommand { get; }
    public AsyncRelayCommand ResetInstanceCommand { get; }
    public AsyncRelayCommand ImportFirmwareCommand { get; }
    public AsyncRelayCommand DeleteFirmwareCommand { get; }
    public AsyncRelayCommand CleanupSessionsCommand { get; }

    public event Func<InstanceRecord, Task>? OpenDeviceRequested;

    /// <summary>Nothing this fires may escape into an async void: the constructor starts it
    /// without a caller that could report a failure.</summary>
    public async Task RefreshAsync()
    {
        try
        {
            // Load/Save take an advisory flock that may spin for seconds, so they stay
            // off the UI thread.
            var records = await Task.Run(_store.Load);
            foreach (var record in records) Instances.AddIfMissing(record);
            foreach (var stale in Instances.Where(item => !records.Contains(item)).ToList()) Instances.Remove(stale);
        }
        catch (Exception error) when (error is IOException or JsonException or InvalidDataException
            or TimeoutException or UnauthorizedAccessException)
        {
            StatusText = error.Message;
        }
        try
        {
            var catalog = await Engine.RunCheckedAsync("library-catalog", Array.Empty<string>());
            Presets.Clear();
            foreach (var preset in catalog["presets"] as JsonArray ?? new JsonArray())
            {
                if (preset is not JsonObject row) continue;
                var model = JsonRead.AsString(row["model"]);
                var name = JsonRead.AsString(row["displayName"]);
                var width = JsonRead.AsInt(row["width"], -1);
                var height = JsonRead.AsInt(row["height"], -1);
                if (model is null || name is null || width < 1 || height < 1)
                    throw new InvalidDataException("固件预设字段无效");
                Presets.Add(new FirmwarePreset(model, name, width, height));
            }
            var items = new List<FirmwareItem>();
            foreach (var entry in catalog["items"] as JsonArray ?? new JsonArray())
            {
                if (entry is not JsonObject row) continue;
                var id = JsonRead.AsString(row["id"]);
                if (id is null) continue;
                items.Add(new FirmwareItem(id,
                    JsonRead.AsString(row["displayName"]) ?? id,
                    JsonRead.AsString(row["model"]) ?? "",
                    JsonRead.AsString(row["firmwareVersion"]) ?? "",
                    JsonRead.AsString(row["otaSHA256"]) ?? "",
                    JsonRead.AsBool(row["availability"]?["ready"]),
                    JsonRead.AsString(row["availability"]?["reason"]) ?? ""));
            }
            FirmwareItems.Clear();
            foreach (var item in items) FirmwareItems.Add(item);
            RaiseCommands();
        }
        catch (Exception error) when (error is IOException or JsonException or InvalidOperationException
            or BridgeRemoteError or InvalidDataException or TimeoutException or UnauthorizedAccessException)
        {
            StatusText = error.Message;
        }
    }

    private async Task OpenDeviceAsync()
    {
        if (_layout is null)
        {
            StatusText = "未找到 Azki Simulator 引擎目录，无法打开设备。";
            return;
        }
        if (SelectedInstance is { } instance && OpenDeviceRequested is { } request) await request(instance);
    }

    public async Task<InstanceRecord?> CreateInstanceAsync(string name, string model, string? profileId,
        string? serial, string? address, bool skipOobe)
    {
        if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(model)) return null;
        var record = new InstanceRecord
        {
            Id = Guid.NewGuid().ToString(),
            Name = name.Trim(),
            Model = model,
            SelectedProfileId = string.IsNullOrWhiteSpace(profileId) ? null : profileId,
            SkipOobe = skipOobe,
            VirtualIdentity = string.IsNullOrWhiteSpace(serial) && string.IsNullOrWhiteSpace(address) ? null
                : new VirtualIdentity { Serial = serial?.Trim(), Address = address?.Trim() },
            CreatedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        };
        try
        {
            // Saving first means a rejected write leaves the visible list untouched.
            var pending = Instances.Append(record).ToList();
            await Task.Run(() => _store.Save(pending));
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException
            or UnauthorizedAccessException)
        {
            StatusText = error.Message;
            return null;
        }
        Instances.Add(record);
        SelectedInstance = record;
        StatusText = $"已创建实例 {record.Name}。";
        return record;
    }

    private async Task DeleteInstanceAsync()
    {
        if (SelectedInstance is not { } instance) return;
        string? cleanupError = null;
        try
        {
            await Engine.RunCheckedAsync("instance-delete", new[] { "--instance-id", instance.Id });
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException)
        {
            cleanupError = error.Message;
        }
        var records = Instances.Where(item => item.Id != instance.Id).ToList();
        try
        {
            _store.Save(records);
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException)
        {
            StatusText = $"实例记录未能删除：{error.Message}";
            return;
        }
        Instances.Remove(instance);
        SelectedInstance = null;
        // Same contract as macOS: the record stays gone and an uncleaned data
        // directory is reported rather than revived.
        if (cleanupError is not null) StatusText = $"实例记录已删除，但未能清理它的数据：{cleanupError}";
    }

    private async Task ResetInstanceAsync()
    {
        if (SelectedInstance is not { } instance) return;
        try
        {
            await Engine.RunCheckedAsync("instance-reset", new[] { "--instance-id", instance.Id });
            StatusText = $"已整理实例 {instance.Name} 的数据（保留已保存快照）。";
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException)
        {
            StatusText = error.Message;
        }
    }

    private Task NewInstanceAsync()
    {
        StatusText = "请在「新建实例」面板中填写。";
        return Task.CompletedTask;
    }

    public async Task ImportFirmwareAsync(IStorageFile? file, string? model)
    {
        if (file is null) return;
        var path = file.Path.LocalPath;
        if (!File.Exists(path))
        {
            StatusText = "选中的固件包不存在或已移动。";
            return;
        }
        try
        {
            var arguments = new List<string> { "--input", path };
            if (!string.IsNullOrEmpty(model)) { arguments.Add("--model"); arguments.Add(model); }
            // The OTA lives outside the sandbox; expose exactly this file read-only
            // and let the catalog write only into the library.
            await Engine.RunCheckedAsync("library-import", arguments,
                libraryWritable: true, readOnlyFiles: new[] { path });
            StatusText = "固件导入完成。";
            await RefreshAsync();
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException
            or UnauthorizedAccessException)
        {
            StatusText = error.Message;
        }
    }

    private Task ImportFirmwareHintAsync()
    {
        StatusText = "请点击「导入固件」选择 OTA 包。";
        return Task.CompletedTask;
    }

    private async Task DeleteFirmwareAsync()
    {
        if (SelectedFirmware is not { } item) return;
        if (item.Digest.Length != 64)
        {
            StatusText = "无法定位该固件的完整 SHA-256。";
            return;
        }
        try
        {
            await Engine.RunCheckedAsync("library-delete", new[] { "--sha256", item.Digest });
            StatusText = "固件已删除。";
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException)
        {
            StatusText = error.Message;
        }
        await RefreshAsync();
    }

    private async Task CleanupSessionsAsync()
    {
        try
        {
            await Engine.RunCheckedAsync("sessions-cleanup", Array.Empty<string>());
            StatusText = "已清理过期临时会话。";
        }
        catch (Exception error) when (error is BridgeRemoteError or TimeoutException or IOException)
        {
            StatusText = error.Message;
        }
    }

    private void RaiseCommands()
    {
        DeleteInstanceCommand.RaiseCanExecute();
        ResetInstanceCommand.RaiseCanExecute();
        OpenDeviceCommand.RaiseCanExecute();
        DeleteFirmwareCommand.RaiseCanExecute();
    }

    public EngineLayout? Layout => _layout;
}

internal static class CollectionExtensions
{
    public static void AddIfMissing<T>(this ObservableCollection<T> items, T value)
    {
        foreach (var existing in items) if (existing?.Equals(value) == true) return;
        items.Add(value);
    }
}
