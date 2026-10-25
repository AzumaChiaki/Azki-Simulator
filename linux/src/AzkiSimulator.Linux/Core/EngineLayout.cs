namespace AzkiSimulator.Linux.Core;

/// <summary>Resolves the packaged Engine/Runtime trees and the bundled interpreters.</summary>
public sealed class EngineLayout
{
    public required string Root { get; init; }
    public required string EngineRoot { get; init; }
    public required string RuntimeRoot { get; init; }

    public string Python => Path.Combine(EngineRoot, "python", "bin", "python3.12");
    public string Cli => Path.Combine(RuntimeRoot, "azki-simulator.py");
    public string Bootstrap => Path.Combine(EngineRoot, "bootstrap.py");
    public string Qemu => Path.Combine(EngineRoot, "project", "backend", "build", "qemu-system-arm");
    public string MinativeLaunch => Path.Combine(RuntimeRoot, "azki_simulator", "minative_launch.py");

    /// <summary>Import recipes live next to Engine, the same sibling layout the CLI
    /// resolves; a sandbox that binds only Engine and Runtime cannot read them.</summary>
    public string FirmwareRoot => Path.Combine(Root, "Firmware");

    /// <summary>Finds the azki-simulator package tree (env AZKI_APP_ROOT wins), then
    /// validates the entrypoints the manifest declares.</summary>
    public static EngineLayout? TryResolve()
    {
        foreach (var candidate in Candidates(AppContext.BaseDirectory))
        {
            var layout = new EngineLayout
            {
                Root = candidate,
                EngineRoot = Path.Combine(candidate, "Engine"),
                RuntimeRoot = Path.Combine(candidate, "Runtime"),
            };
            if (layout.IsComplete()) return layout;
        }
        return null;
    }

    private static IEnumerable<string> Candidates(string baseDirectory)
    {
        var fromEnvironment = Environment.GetEnvironmentVariable("AZKI_APP_ROOT");
        if (!string.IsNullOrEmpty(fromEnvironment)) yield return Path.TrimEndingDirectorySeparator(fromEnvironment);
        var directory = new DirectoryInfo(baseDirectory);
        while (directory is not null)
        {
            yield return directory.FullName;
            directory = directory.Parent;
        }
    }

    private bool IsComplete() =>
        File.Exists(Python) && File.Exists(Cli) && File.Exists(Bootstrap) && File.Exists(Qemu)
        && File.Exists(MinativeLaunch) && File.Exists(Path.Combine(FirmwareRoot, "catalog.json"));
}
