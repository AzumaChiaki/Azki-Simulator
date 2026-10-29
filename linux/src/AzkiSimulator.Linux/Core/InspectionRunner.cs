using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

public sealed record CliResult(int ExitCode, JsonNode? Payload, string Raw, string Error = "");

/// <summary>
/// Runs one-shot azki-simulator.py commands inside bwrap, mirroring
/// StandaloneInspectionExecutor: a private 0700 azki-cmd-* work directory, a
/// scrubbed environment, merged bounded output and hard timeouts.
/// </summary>
public sealed class InspectionRunner(EngineLayout Layout)
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(15);
    private static readonly Dictionary<string, TimeSpan> Timeouts = new()
    {
        ["library-import"] = TimeSpan.FromSeconds(300),
        ["sessions-cleanup"] = TimeSpan.FromSeconds(90),
        ["prepare-bundled"] = TimeSpan.FromSeconds(45),
        ["library-probe"] = TimeSpan.FromSeconds(45),
        ["instance-reset"] = TimeSpan.FromSeconds(45),
    };

    public async Task<CliResult> RunAsync(string action, IEnumerable<string> extraArguments,
        CancellationToken cancellationToken = default)
    {
        var argv = extraArguments.ToList();
        var work = DataLocations.NewCommandWorkRoot();
        DataLocations.EnsurePrivateDirectory(work);
        DataLocations.EnsurePrivateDirectory(DataLocations.FirmwareLibraryRoot);
        Directory.CreateDirectory(DataLocations.InstancesRoot);
        try
        {
            var policy = new SandboxPolicy
            {
                ReadOnlyTrees =
                {
                    Layout.EngineRoot, Layout.RuntimeRoot, Layout.FirmwareRoot,
                    DataLocations.InstancesRoot, DataLocations.FirmwareLibraryRoot,
                },
                ReadWriteTrees = { work },
            };
            CliGrants.Apply(policy, action, argv, DataLocations.FirmwareLibraryRoot);
            // bwrap binds an existing source path, so a grant the command needs before it
            // can create the folder itself has to be created here, as the reference host does.
            foreach (var tree in policy.ReadWriteTrees.Distinct()) DataLocations.EnsurePrivateDirectory(tree);

            var info = new ProcessStartInfo
            {
                FileName = SandboxPolicy.Launcher,
                WorkingDirectory = work,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            foreach (var argument in policy.BuildArguments()) info.ArgumentList.Add(argument);
            info.ArgumentList.Add(Layout.Python);
            foreach (var argument in new[] { "-I", "-S", "-B", Layout.Cli, action,
                                             "--engine-root", Layout.EngineRoot, "--library-root", DataLocations.FirmwareLibraryRoot })
                info.ArgumentList.Add(argument);
            foreach (var argument in argv) info.ArgumentList.Add(argument);
            info.Environment.Clear();
            info.Environment["PATH"] = "/usr/bin:/bin";
            info.Environment["LANG"] = "C";
            info.Environment["LC_ALL"] = "C";
            info.Environment["HOME"] = work;
            info.Environment["TMPDIR"] = work;
            info.Environment["PYTHONDONTWRITEBYTECODE"] = "1";
            info.Environment["AZKI_HOST_TIMEZONE"] = TimeZoneInfo.Local.Id;

            using var process = Process.Start(info) ?? throw new InvalidOperationException("无法启动检查进程");
            var stdoutTask = process.StandardOutput.ReadToEndAsync();
            var stderrTask = process.StandardError.ReadToEndAsync();
            using var timeout = new CancellationTokenSource(Timeouts.GetValueOrDefault(action, DefaultTimeout));
            try
            {
                // The pipes are drained before the wait, not after: the sandboxed payload
                // inherits them, and a process object that goes away closes them, so a
                // payload still writing at that moment is killed instead of finishing.
                // Waiting for the streams first is also what .NET documents for redirected
                // output, and the hard deadline still covers both waits.
                await Task.WhenAll(stdoutTask, stderrTask).WaitAsync(timeout.Token);
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                // Killing the child closes the pipes, so the reads are awaited rather than
                // abandoned as unobserved task exceptions.
                await QuietlyDrain(stdoutTask, stderrTask);
                // A caller cancellation stays a cancellation; only the deadline is a timeout.
                if (cancellationToken.IsCancellationRequested) throw;
                throw new TimeoutException($"命令 {action} 超时");
            }
            var stdout = await stdoutTask;
            var stderr = await stderrTask;
            var raw = stdout.Length == 0 ? stderr : stdout;
            JsonNode? payload = null;
            try { payload = JsonNode.Parse(raw); } catch (JsonException) { /* non-JSON output surfaces through Raw */ }
            return new CliResult(process.ExitCode, payload, raw, stderr);
        }
        finally
        {
            // The child writes scratch files under TMPDIR=work, so deleting only an empty
            // directory would leak one azki-cmd-* tree per command into /tmp forever.
            try { if (DataLocations.IsCommandWorkRoot(work)) Directory.Delete(work, recursive: true); }
            catch (IOException) { }
        }
    }

    private static async Task QuietlyDrain(Task<string> stdout, Task<string> stderr)
    {
        try { await Task.WhenAll(stdout, stderr); } catch (Exception) { /* the child died mid-read */ }
    }

    /// <summary>Runs an action and requires ok:true; failures throw BridgeRemoteError with the CLI message.</summary>
    public async Task<JsonNode> RunCheckedAsync(string action, IEnumerable<string> extraArguments,
        CancellationToken cancellationToken = default)
    {
        var result = await RunAsync(action, extraArguments, cancellationToken);
        if (result.Payload is not JsonObject body || !JsonRead.AsBool(body["ok"]))
        {
            var message = JsonRead.AsString(result.Payload?["error"]?["message"]);
            // The launcher writes its own refusals to stderr while the command writes
            // JSON to stdout, so both are needed to say why a command did not run.
            var detail = string.IsNullOrEmpty(result.Error) ? result.Raw : result.Error;
            if (string.IsNullOrEmpty(detail) && !string.IsNullOrEmpty(result.Raw)) detail = result.Raw;
            if (string.IsNullOrEmpty(detail))
            {
                // 128 + signal is how a killed process reports itself. On this machine the
                // kernel's OOM killer is the usual reason, and saying so beats a bare code.
                detail = result.ExitCode > 128
                    ? $"命令被信号 {result.ExitCode - 128} 终止（退出码 {result.ExitCode}，常见于内存压力下的系统 OOM）"
                    : $"退出码 {result.ExitCode}";
            }
            throw new BridgeRemoteError(message ?? $"{action} 失败：{(detail.Length > 400 ? detail[..400] : detail)}");
        }
        return body;
    }
}

public sealed class BridgeRemoteError(string message) : Exception(message);
