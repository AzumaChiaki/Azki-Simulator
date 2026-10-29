namespace AzkiSimulator.Linux.Core;

/// <summary>XDG data roots, mirroring the macOS SimulatorDataLocations namespace.</summary>
public static class DataLocations
{
    public const string BundleId = "com.azki.AzkiSimulator";

    private static string DataRoot =>
        Environment.GetEnvironmentVariable("AZKI_DATA_ROOT")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share", "AzkiSimulator");

    // Mirrors SimulatorDataLocations: Instances sits under "AzkiSimulator",
    // the firmware library under the bundle identifier.
    public static string InstancesRoot => Path.Combine(DataRoot, "AzkiSimulator", "Instances");
    public static string FirmwareLibraryRoot => Path.Combine(DataRoot, BundleId, "FirmwareLibrary");
    public static string AppSupportRoot => Path.Combine(DataRoot, BundleId);
    public static string GuiLogPath => Path.Combine(AppSupportRoot, "logs", "gui.log");
    public static string TmpRoot => Environment.GetEnvironmentVariable("AZKI_TMP_ROOT") ?? "/tmp";

    // The plan pins settings.json at ~/.config/azki-simulator, separate from the
    // XDG data root: settings are per-login preferences, not instance content.
    public static string ConfigRoot =>
        Environment.GetEnvironmentVariable("AZKI_CONFIG_ROOT")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "azki-simulator");

    public static string SettingsPath => Path.Combine(ConfigRoot, "settings.json");

    /// <summary>Methods, not properties: every call hands out a different directory, so
    /// reading the same name twice must not look like it returns the same value.</summary>
    public static string NewSessionRoot() =>
        Path.Combine(TmpRoot, $"azki-b-{Guid.NewGuid().ToString("N")[..12]}");

    public static string NewCommandWorkRoot() =>
        Path.Combine(TmpRoot, $"azki-cmd-{Guid.NewGuid().ToString("N")[..12]}");

    public static bool IsCommandWorkRoot(string path) =>
        Path.GetFileName(Path.TrimEndingDirectorySeparator(path)).StartsWith("azki-cmd-", StringComparison.Ordinal);

    public static void EnsurePrivateDirectory(string path)
    {
        // An existing path may belong to someone else (a shared AZKI_TMP_ROOT), and
        // CreateDirectory would accept it silently, so ownership is asserted first.
        if (Directory.Exists(path))
        {
            var stat = UnixNative.Stat(path);
            if (stat.Uid != UnixNative.getuid())
                throw new IOException($"目录已存在且不属于当前用户：{path}");
            MakePrivate(path);
            return;
        }
        // Creating a private directory also creates the ancestors it is missing, and
        // those come out at the umask default: the guest run directory was left 0755
        // as a side effect of creating its "tmp" child, which the session helper then
        // refused. Every component this call brings into existence is made private.
        var missing = new List<string>();
        for (var walk = Path.GetFullPath(path); !Directory.Exists(walk);)
        {
            missing.Add(walk);
            var parent = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(walk));
            if (string.IsNullOrEmpty(parent)) break;
            walk = parent;
        }
        Directory.CreateDirectory(path);
        MakePrivate(path);
        foreach (var created in missing) MakePrivate(created);
    }

    private static void MakePrivate(string path) =>
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
}
