namespace AzkiSimulator.Linux.Core;

/// <summary>bwrap argument construction for every supervised process class.</summary>
/// <remarks>
/// One translation of the macOS Seatbelt posture: deny-default visibility with
/// explicit read-only and read-write trees, /proc, a minimal /dev, no network, and
/// a deliberately shared pid namespace so kernel peer credentials and process-group
/// reaping keep host-visible identities — the same contract
/// minative_isolation.bwrap_prefix implements in Python. bwrap offers no
/// "--share-pid", so the namespaces are enumerated instead of using --unshare-all.
/// Missing sources fail the launch; there is no unsandboxed fallback.
/// Granularity note (documented deviation): bound read-only trees expose all
/// their contents, not only manifest-pinned files; the trees contain only
/// application-owned material.
/// </remarks>
public sealed class SandboxPolicy
{
    public List<string> ReadOnlyTrees { get; init; } = new();
    public List<string> ReadWriteTrees { get; init; } = new();
    public List<string> ReadOnlyFiles { get; init; } = new();

    /// <summary>Directories re-bound read-only *after* a writable parent, so a command
    /// granted a folder cannot reach the siblings inside it. Only meaningful as a
    /// restriction on a writable tree; a bind is applied in argument order.</summary>
    public List<string> ProtectedTrees { get; init; } = new();

    /// <summary>bwrap flags only: callers put Launcher in ProcessStartInfo.FileName,
    /// so repeating it here would make bwrap try to exec itself as the payload.</summary>
    public List<string> BuildArguments()
    {
        var args = new List<string>
        {
            "--die-with-parent", "--new-session",
            "--unshare-user", "--unshare-ipc", "--unshare-uts", "--unshare-cgroup", "--unshare-net",
            "--tmpfs", "/", "--dev", "/dev", "--proc", "/proc",
            // bwrap's root is empty, so the usr-merge links have to exist: binaries
            // record /lib64/ld-linux-x86-64.so.2 as their interpreter and exec fails
            // with ENOENT without it, even though /usr is bound.
            "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/lib", "/lib", "--symlink", "usr/bin", "/bin",
            "--ro-bind", "/usr", "/usr", "--ro-bind", "/etc", "/etc", "--tmpfs", "/tmp",
        };
        // A tree the command may write must not also be bound read-only at the same path:
        // the later bind would win, and the duplicate mount only makes the argv harder to read.
        var writable = ReadWriteTrees.Distinct().ToList();
        foreach (var tree in ReadOnlyTrees.Distinct().Except(writable)) args.AddRange(new[] { "--ro-bind", tree, tree });
        foreach (var file in ReadOnlyFiles.Distinct()) args.AddRange(new[] { "--ro-bind", file, file });
        foreach (var tree in writable) args.AddRange(new[] { "--bind", tree, tree });
        // Last so they win over the writable parent: what is bound here is visible but
        // cannot be unlinked, because a mount point is not removable from inside.
        foreach (var tree in ProtectedTrees.Distinct()) args.AddRange(new[] { "--ro-bind", tree, tree });
        args.Add("--");
        return args;
    }

    public const string Launcher = "/usr/bin/bwrap";
    public static bool Available => File.Exists(Launcher);
}
