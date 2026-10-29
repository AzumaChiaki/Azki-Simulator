using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;

namespace AzkiSimulator.Linux.Tests;

/// <summary>Runs the shipped Python host layer under the same interpreter flags the
/// packaged runtime uses, so a contract test can ask the real validators rather than a
/// re-implementation of them.</summary>
internal static class PythonProbe
{
    public static string? RuntimeDirectory()
    {
        for (var probe = new DirectoryInfo(System.AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, "Sources/AzkiSimulator/Runtime");
            if (File.Exists(Path.Combine(candidate, "azki_simulator/native_broker.py"))) return candidate;
        }
        return null;
    }

    public static async Task<(int ExitCode, string Stdout, string Stderr)> RunAsync(string script, params string[] rest)
    {
        var startInfo = new ProcessStartInfo("/usr/bin/python3")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var flag in new[] { "-I", "-S", "-B" }) startInfo.ArgumentList.Add(flag);
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(script);
        foreach (var value in rest) startInfo.ArgumentList.Add(value);
        using var process = Process.Start(startInfo)!;
        var stdout = await process.StandardOutput.ReadToEndAsync();
        var stderr = await process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, stdout, stderr);
    }

    /// <summary>Embeds a C# value as a JSON literal the script can read back.</summary>
    public static string Literal(object? value) => JsonSerializer.Serialize(value);
}
