using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>One host status sharing session found on disk. Summary and PathLine
/// exist so the view binds plain strings and no converter is needed.</summary>
public sealed record HostStatusSession(string Id, string Directory, string? ApplicationVersion,
    string Protocol, IReadOnlyList<string> Capabilities, int LeaseSequence, bool LeaseActive)
{
    private string ProtocolShort => Protocol.EndsWith("studio-files.v1") ? "studio v1" : "status v1";
    public string Summary => $"{ProtocolShort}"
        + (ApplicationVersion is { } version ? $" · {version}" : "")
        + $" · 租约 {(LeaseActive ? "有效" : "已过期")} · 序号 {LeaseSequence} · {string.Join("/", Capabilities)}";
    public string PathLine => Directory;
}

/// <summary>
/// Read-only scan of the azki.simulator.status-files.v1 sharing that the macOS
/// app publishes under its tmp root (SimulatorStatusFilePublisher.swift). The
/// approved Linux scope is 读取展示 only: this never writes, never sends a
/// request and never dispatches to any emulator service. Entries that fail the
/// publisher's own shape are skipped silently — they may be stale or belong to
/// other tooling, and a damaged shared directory must not break the manager UI.
/// </summary>
public static class HostStatusReader
{
    private const string DirectoryPrefix = "azki-host-";
    private static readonly string[] PermittedProtocols =
    [
        "azki.simulator.status-files.v1",
        "azki.simulator.studio-files.v1",
    ];
    // The publisher's lease is five seconds with a session cap of thirty minutes;
    // a far-future expiry is not a live lease but a forged or broken one.
    private const int LeaseSeconds = 5;
    private const int MaximumSessions = 64;

    public static IReadOnlyList<HostStatusSession> Scan(string tmpRoot)
    {
        var results = new List<HostStatusSession>();
        var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        try
        {
            if (!Directory.Exists(tmpRoot)) return results;
            foreach (var name in Directory.GetDirectories(tmpRoot, DirectoryPrefix + "*").OrderBy(path => path))
            {
                if (results.Count >= MaximumSessions) break;
                try
                {
                    var session = Read(name, now);
                    if (session is not null) results.Add(session);
                }
                catch (Exception error) when (error is IOException or JsonException or InvalidOperationException
                    or UnauthorizedAccessException or PlatformNotSupportedException)
                {
                    // A session that cannot be validated at all is just not shown.
                }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // The tmp root itself may be unreadable (AZKI_TMP_ROOT pointing elsewhere);
            // an empty list with no refresh result is the honest answer.
        }
        return results;
    }

    private static HostStatusSession? Read(string path, long now)
    {
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(path));
        var id = name[DirectoryPrefix.Length..];
        // The publisher names its directory "azki-host-" plus the lowercase UUID and
        // nothing else; any other suffix is not one of these sessions.
        if (!Guid.TryParseExact(id, "D", out var parsed) || id != parsed.ToString("D").ToLowerInvariant())
            return null;
        // The directory itself is only opened without following a symlink: the trust
        // comes from the files inside it, which OpenRegularFile requires to be ours,
        // single-link, regular and unwritable by anyone else. A directory planted
        // by another user therefore cannot forge a session — its files are not ours.
        var directory = UnixNative.OpenDirectory(path);
        using (directory)
        {
            using var manifestStream = UnixNative.OpenRegularFile(directory, "session.json", 16_384);
            if (manifestStream is null) return null;
            using var reader = new StreamReader(manifestStream, System.Text.Encoding.UTF8);
            var manifest = JsonNode.Parse(reader.ReadToEnd())?.AsObject();
            if (manifest is null || JsonRead.AsInt(manifest["version"], -1) != 1
                || JsonRead.AsString(manifest["id"]) != id
                || JsonRead.AsString(manifest["application"]) != "AzkiSimulator"
                || JsonRead.AsString(manifest["transport"]) != "file-queue"
                || JsonRead.AsString(manifest["lease"]) != "lease.json"
                || !PermittedProtocols.Contains(JsonRead.AsString(manifest["protocol"]))
                || Path.GetFullPath(JsonRead.AsString(manifest["directory"]) ?? "")
                   != Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)))
                return null;
            // session.json carries no application version (the publisher only
            // reveals it in a status response); absence is shown, not invented.
            var applicationVersion = JsonRead.AsString(manifest["applicationVersion"]);
            var capabilities = JsonRead.AsStringList(manifest["capabilities"])
                .Where(entry => entry.Length <= 64).Take(8).ToList();
            if (capabilities.Count == 0) return null;

            using var leaseStream = UnixNative.OpenRegularFile(directory, "lease.json", 4_096);
            if (leaseStream is null) return null;
            using var leaseReader = new StreamReader(leaseStream, System.Text.Encoding.UTF8);
            var lease = JsonNode.Parse(leaseReader.ReadToEnd())?.AsObject();
            if (lease is null || JsonRead.AsInt(lease["version"], -1) != 1 || JsonRead.AsString(lease["id"]) != id)
                return null;
            var sequence = JsonRead.AsInt(lease["sequence"], -1);
            var expiresAt = JsonRead.AsInt(lease["expiresAt"], -1);
            if (sequence < 1 || expiresAt < 0) return null;
            // A publisher never leases beyond its own lease window; a far-future
            // expiry is not a live session but a forged or broken one, and is hidden.
            if (expiresAt > now + LeaseSeconds) return null;
            var active = now < expiresAt;
            return new HostStatusSession(id, Path.TrimEndingDirectorySeparator(path), applicationVersion,
                JsonRead.AsString(manifest["protocol"])!, capabilities, sequence, active);
        }
    }
}
