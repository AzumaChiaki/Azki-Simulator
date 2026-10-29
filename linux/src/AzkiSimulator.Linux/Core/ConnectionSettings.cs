using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Host-wide connection settings, the Linux counterpart of ConnectionSettings.swift:
/// same default, same accepted range, same UserDefaults key name, persisted to
/// settings.json under the XDG config root instead of UserDefaults.
/// </summary>
public sealed class ConnectionSettings : ObservableObject
{
    public static readonly ConnectionSettings Shared = new();

    public const int DefaultTimeoutSeconds = 30;
    public const int MinimumTimeoutSeconds = 1;
    public const int MaximumTimeoutSeconds = 300;

    /// <summary>Keeps the Swift UserDefaults key verbatim, so the settings file
    /// stays greppable against the macOS implementation it mirrors.</summary>
    public const string TimeoutKey = "simulator.connectionTimeoutSeconds";

    private readonly object _diskGate = new();
    private readonly string? _path;
    private int _timeoutSeconds = DefaultTimeoutSeconds;

    public ConnectionSettings(string? path = null)
    {
        _path = path ?? DataLocations.SettingsPath;
        _timeoutSeconds = ReadTimeoutSeconds(_path);
    }

    public int TimeoutSeconds
    {
        get => Volatile.Read(ref _timeoutSeconds);
        set
        {
            // The Swift didSet rejects an out-of-range write and keeps the old value;
            // a stored 0 or 500 would otherwise survive into every future deadline.
            if (value is < MinimumTimeoutSeconds or > MaximumTimeoutSeconds) return;
            if (!Set(ref _timeoutSeconds, value)) return;
            Persist(value);
        }
    }

    /// <summary>Backs the numeric spinner; re-raises so a rejected edit snaps the
    /// UI back to the value actually stored.</summary>
    public double TimeoutSecondsDisplay
    {
        get => TimeoutSeconds;
        set
        {
            if (double.IsFinite(value)) TimeoutSeconds = (int)Math.Round(value);
            Raise();
        }
    }

    private static int ReadTimeoutSeconds(string? path)
    {
        if (path is null) return DefaultTimeoutSeconds;
        var fallback = DefaultTimeoutSeconds;
        try
        {
            if (!File.Exists(path)) return fallback;
            var node = JsonNode.Parse(File.ReadAllText(path))?.AsObject();
            var stored = JsonRead.AsInt(node?[TimeoutKey], fallback);
            return stored is >= MinimumTimeoutSeconds and <= MaximumTimeoutSeconds ? stored : fallback;
        }
        catch (Exception error) when (error is IOException or JsonException
            or InvalidOperationException or UnauthorizedAccessException)
        {
            // A damaged settings file must never brick the app: read it, ignore it,
            // and let the next successful write replace it.
            UiFault.Report(error);
            return fallback;
        }
    }

    private void Persist(int value)
    {
        if (_path is null) return;
        var directory = Path.GetDirectoryName(_path);
        if (directory is null) return;
        lock (_diskGate)
        {
            try
            {
                DataLocations.EnsurePrivateDirectory(directory);
                var document = new JsonObject { ["version"] = 1, [TimeoutKey] = value };
                UnixNative.PublishRecord(directory, "settings", Path.GetFileName(_path),
                    document.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), mayReplace: true);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                // The in-memory value already changed; losing persistence is reported,
                // not fatal, and the next write retries.
                UiFault.Report(error);
            }
        }
    }
}
