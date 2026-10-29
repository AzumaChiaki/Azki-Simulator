using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Finishes the one thing a sandboxed sessions-cleanup pass cannot do itself.
///
/// bwrap grants a stale root by binding it, and a mount point cannot be rmdir'd from
/// inside the namespace, so the sandbox empties the tree and reports
/// "remove-failed: Device or resource busy" for the directory it has already decided is
/// safe to lose. Handing the sandbox the temp root itself would fix that only by letting
/// the command unlink every entry this user owns there, which is broader than the
/// name-prefix rule the reference grant expresses. So the host takes the empty shell —
/// and only for paths the sandbox named, which is what keeps a root that is merely being
/// prepared (empty, recent, deliberately kept) out of reach.
/// </summary>
internal static class SessionCleanup
{
    public static IReadOnlyList<string> Reclaim(JsonNode? receipt, IEnumerable<string> candidates)
    {
        var granted = candidates.Select(Path.TrimEndingDirectorySeparator).ToHashSet();
        var blocked = (receipt?["kept"] as JsonArray ?? new JsonArray()).OfType<JsonNode>()
            .Select(node => Path.TrimEndingDirectorySeparator(JsonRead.AsString(node) ?? ""))
            .Where(path => path.Length > 0
                && JsonRead.AsString(receipt!["reasons"]?[path])?.StartsWith("remove-failed", StringComparison.Ordinal) == true)
            .ToHashSet();
        var removed = new List<string>();
        foreach (var path in blocked.Intersect(granted))
        {
            var root = Path.TrimEndingDirectorySeparator(DataLocations.TmpRoot);
            if (Path.GetDirectoryName(path) != root) continue;
            var name = Path.GetFileName(path);
            if (!CliGrants.SessionPrefixes.Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal))) continue;
            if (!UnixNative.TryStat(path, out var stat, out var error) || error is not null
                || !UnixNative.IsDirectory(stat.Mode) || stat.Uid != UnixNative.getuid()) continue;
            List<string> children;
            try { children = Directory.EnumerateFileSystemEntries(path).ToList(); }
            catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) { continue; }
            if (children.Count > 0) continue;
            try { Directory.Delete(path); removed.Add(path); }
            catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) { }
        }
        return removed;
    }
}
