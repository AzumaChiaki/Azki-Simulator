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
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            return;
        }
        Directory.CreateDirectory(path);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    }
}
