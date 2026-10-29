using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using Microsoft.Win32.SafeHandles;

namespace AzkiSimulator.Linux.Core;

public sealed record VirtualIdentity
{
    [JsonPropertyName("serial")] public string? Serial { get; init; }
    [JsonPropertyName("address")] public string? Address { get; init; }
}

public sealed record InstanceRecord
{
    [JsonPropertyName("id")] public required string Id { get; init; }
    [JsonPropertyName("name")] public required string Name { get; init; }
    [JsonPropertyName("model")] public required string Model { get; init; }
    [JsonPropertyName("selectedProfileID")] public string? SelectedProfileId { get; init; }
    [JsonPropertyName("virtualIdentity")] public VirtualIdentity? VirtualIdentity { get; init; }
    [JsonPropertyName("skipOOBE")] public bool? SkipOobe { get; init; }
    [JsonPropertyName("createdAt")] public required double CreatedAt { get; init; }
}

/// <summary>
/// instances.json with the macOS record schema (field names preserved).
/// Access is serialized by an advisory flock on .instances.lock, mirroring
/// SimulatorInstanceRecordStore; malformed old data is never silently reset.
/// </summary>
public sealed class InstanceStore
{
    public const int MaximumRecords = 128;
    public const int MaximumFileBytes = 131_072;
    private static readonly string Filename = "instances.json";
    private static readonly string LockName = ".instances.lock";
    private readonly object _gate = new();

    public string Root { get; init; } = DataLocations.InstancesRoot;

    public IReadOnlyList<InstanceRecord> Load()
    {
        lock (_gate)
        {
            EnsureRoot();
            using var diskLock = Lock();
            var path = Path.Combine(Root, Filename);
            if (!File.Exists(path)) return [];
            return ParseDocument(File.ReadAllText(path));
        }
    }

    public void Save(IReadOnlyList<InstanceRecord> records)
    {
        if (records.Count > MaximumRecords) throw new InvalidOperationException($"实例记录超过 {MaximumRecords} 条");
        foreach (var record in records)
        {
            if (string.IsNullOrEmpty(record.Id) || string.IsNullOrEmpty(record.Name) || string.IsNullOrEmpty(record.Model))
                throw new InvalidOperationException("实例记录无效");
        }
        var document = new JsonObject { ["version"] = 1, ["records"] = JsonSerializer.SerializeToNode(records) };
        var bytes = document.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        if (System.Text.Encoding.UTF8.GetByteCount(bytes) > MaximumFileBytes) throw new InvalidOperationException("实例记录文件超过 128 KiB");
        lock (_gate)
        {
            EnsureRoot();
            using var diskLock = Lock();
            var path = Path.Combine(Root, Filename);
            var existed = File.Exists(path);
            if (existed) ParseDocument(File.ReadAllText(path)); // refuse to overwrite malformed data
            UnixNative.PublishRecord(Root, "instances", Filename, bytes, mayReplace: existed);
        }
    }

    private static IReadOnlyList<InstanceRecord> ParseDocument(string text)
    {
        var node = JsonNode.Parse(text)?.AsObject() ?? throw new InvalidDataException("实例记录不是 JSON 对象");
        if (node["version"]?.GetValue<int>() != 1) throw new InvalidDataException("实例记录版本不受支持");
        var records = node["records"]?.AsArray() ?? throw new InvalidDataException("实例记录缺少 records");
        if (records.Count > MaximumRecords) throw new InvalidDataException("实例记录过多");
        return records.Select(item => JsonSerializer.Deserialize<InstanceRecord>(item!.AsObject().ToJsonString())!).ToList();
    }

    private void EnsureRoot()
    {
        Directory.CreateDirectory(Root);
        File.SetUnixFileMode(Root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    }

    private FileStream Lock()
    {
        var stream = new FileStream(Path.Combine(Root, LockName), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        try
        {
            File.SetUnixFileMode(Path.Combine(Root, LockName), UnixFileMode.UserRead | UnixFileMode.UserWrite);
            var deadline = (MonotonicClock.Now + TimeSpan.FromSeconds(10));
            while (UnixNative.flock((int)stream.SafeFileHandle.DangerousGetHandle(),
                       UnixNative.LockEx | UnixNative.LockNb) != 0)
            {
                if (MonotonicClock.Now > deadline) throw new TimeoutException("无法获得实例记录锁");
                Thread.Sleep(20);
            }
            return stream;
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }
}
