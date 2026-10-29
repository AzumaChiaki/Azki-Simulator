using System.Text.RegularExpressions;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// The per-action write grants for the one-shot CLI, mirroring
/// StandaloneProcessSandbox.makeInspection. The grant is derived from the argv itself, so a
/// caller can neither forget it (the command then dies on a read-only tree inside the
/// sandbox) nor overstate it (an instance-settings save would otherwise make the whole
/// firmware library writable). Anything the reference does not allow is refused here too.
/// </summary>
internal static class CliGrants
{
    private static readonly Regex InstanceIdShape = new("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$",
        RegexOptions.Compiled);

    /// <summary>The session roots this app creates under the platform temp root; the same
    /// prefixes the reference cleanup rule names.</summary>
    public static readonly string[] SessionPrefixes = { "azki-b-", "azki-sim-", "azki-cmd-" };

    public static string InstanceFolder(string libraryRoot, string instanceId) =>
        System.IO.Path.Combine(libraryRoot, "instances", instanceId);

    public static void Apply(SandboxPolicy policy, string action, IReadOnlyList<string> argv, string libraryRoot)
    {
        switch (action)
        {
            case "library-catalog":
                break;
            case "library-import":
            case "library-delete":
            case "library-delete-all":
                policy.ReadWriteTrees.Add(libraryRoot);
                break;
            case "instance-settings":
            case "instance-reset":
                policy.ReadWriteTrees.Add(InstanceFolder(libraryRoot, InstanceId(argv, action)));
                break;
            case "instance-delete":
                // This command removes the instance folder itself, and a folder bwrap binds
                // is a mount point: unlinking it fails EBUSY, and while its parent arrives
                // read-only even rmdir fails (EROFS) — both confirmed against a real bwrap.
                // The grant is therefore the parent, with every sibling re-bound read-only on
                // top, because that same mount property is what stops the wider grant from
                // letting this command delete somebody else's instance.
                DeleteGrant(policy, libraryRoot, InstanceId(argv, action));
                break;
            case "prepare-bundled":
                // The reference makes the instance identity optional here: a
                // preparation without one gets the session root and no instance
                // folder. Linux follows that shape, so the instance grant is
                // made only when the identity is actually there — and a
                // malformed one is still refused before anything is granted.
                if (Value(argv, "--instance-id") is not null)
                    policy.ReadWriteTrees.Add(InstanceFolder(libraryRoot, InstanceId(argv, action)));
                policy.ReadWriteTrees.Add(Required(argv, "--session-root", action));
                break;
            case "sessions-cleanup":
                // /tmp inside the sandbox is a private tmpfs, so the cleanup can only reach
                // the roots this host enumerates for it; the reference grants the same name
                // prefixes and nothing else.
                foreach (var root in StaleSessionRoots()) policy.ReadWriteTrees.Add(root);
                break;
            default:
                throw new BridgeRemoteError($"未授权的静态命令：{action}");
        }
        var input = Value(argv, "--input");
        if (input is not null) policy.ReadOnlyFiles.Add(input);
    }

    /// <summary>The reference validates the instance UUID before granting anything: a name
    /// outside that shape would widen the bind to a sibling folder.</summary>
    private static string InstanceId(IReadOnlyList<string> argv, string action)
    {
        var value = Required(argv, "--instance-id", action);
        if (!InstanceIdShape.IsMatch(value)) throw new BridgeRemoteError($"实例身份无效：{action}");
        return value;
    }

    /// <summary>The instance container, with every sibling folder re-bound read-only: the
    /// command needs write access to the parent to remove its own directory, and the mount
    /// property that makes a bound directory unremovable is exactly what keeps that wider
    /// grant from letting it delete somebody else's instance.</summary>
    private static void DeleteGrant(SandboxPolicy policy, string libraryRoot, string instanceId)
    {
        var instance = Trim(InstanceFolder(libraryRoot, instanceId));
        var container = System.IO.Path.GetDirectoryName(instance)!;
        List<string> siblings;
        try
        {
            siblings = Directory.EnumerateDirectories(container)
                .Where(entry => !string.Equals(Trim(entry), instance, StringComparison.Ordinal))
                .ToList();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // An unreadable container means the siblings cannot be named, and a grant
            // without that list would be the broad one. Fall back to the narrow folder: the
            // command then fails on its own removal rather than running over guessed policy.
            policy.ReadWriteTrees.Add(instance);
            return;
        }
        policy.ReadWriteTrees.Add(container);
        foreach (var sibling in siblings) policy.ProtectedTrees.Add(Trim(sibling));
    }

    private static string Trim(string path) => System.IO.Path.TrimEndingDirectorySeparator(path);

    private static string Required(IReadOnlyList<string> argv, string flag, string action) =>
        Value(argv, flag) ?? throw new BridgeRemoteError($"命令 {action} 缺少 {flag}");

    private static string? Value(IReadOnlyList<string> argv, string flag)
    {
        for (var index = 0; index + 1 < argv.Count; index++)
        {
            if (argv[index] == flag) return argv[index + 1];
        }
        return null;
    }

    /// <summary>The stale session roots on this host right now. The cleanup grant binds
    /// exactly these, and the caller passes the same list back to SessionCleanup so the
    /// host only ever considers a directory the sandbox was shown.</summary>
    public static IEnumerable<string> StaleSessionRoots()
    {
        string[] entries;
        try { entries = Directory.GetDirectories(DataLocations.TmpRoot); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return Array.Empty<string>();
        }
        // Only roots this user owns: binding somebody else's directory would make the
        // pre-launch permission check fail, and cleanup would stop working for good.
        return entries.Where(entry => SessionPrefixes.Any(prefix =>
                System.IO.Path.GetFileName(entry).StartsWith(prefix, StringComparison.Ordinal))
            && UnixNative.TryStat(entry, out var stat, out _) && stat.Uid == UnixNative.getuid());
    }
}
