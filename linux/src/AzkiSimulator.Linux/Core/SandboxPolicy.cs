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
        foreach (var tree in ReadOnlyTrees.Distinct()) args.AddRange(new[] { "--ro-bind", tree, tree });
        foreach (var file in ReadOnlyFiles.Distinct()) args.AddRange(new[] { "--ro-bind", file, file });
        foreach (var tree in ReadWriteTrees.Distinct()) args.AddRange(new[] { "--bind", tree, tree });
        args.Add("--");
        return args;
    }

    public const string Launcher = "/usr/bin/bwrap";
    public static bool Available => File.Exists(Launcher);
}
