using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Owns one prepared session: the helper (H) serving control.sock and the
/// guest supervisor (G) brokered through native-broker files, mirroring
/// StandaloneRuntimeModel + StandaloneNativeLaunch + StandaloneGuestBroker.
/// Both classes run under bwrap; failed spawns are hard errors with no
/// unsandboxed fallback.
/// </summary>
public sealed class SessionSupervisor : IAsyncDisposable
{
    private readonly EngineLayout _layout;
    private readonly InspectionRunner _inspection;
    private readonly string _sessionRoot;
    private readonly string _sessionDirectory;
    private readonly string _brokerDirectory;
    private Process? _helper;
    private volatile Process? _guest;
    private LogWriter? _helperLog;
    // Written by the broker watcher, read by teardown on another thread.
    private volatile LogWriter? _guestLog;
    private BridgeClient? _client;
    private CancellationTokenSource? _watchCancellation;
    private Task? _watchTask;
    private string _runName = "";
    private volatile bool _publishedTerminal;

    private SessionSupervisor(EngineLayout layout, InspectionRunner inspection, string sessionRoot)
    {
        _layout = layout;
        _inspection = inspection;
        _sessionRoot = sessionRoot;
        _sessionDirectory = Path.Combine(sessionRoot, "session");
        _brokerDirectory = Path.Combine(_sessionDirectory, "native-broker");
    }

    public BridgeClient Client => _client ?? throw new InvalidOperationException("会话尚未启动");
    public string SessionDirectory => _sessionDirectory;

    /// <summary>Full start choreography. Returns after the guest supervisor is launched (boot continues async).</summary>
    public static async Task<SessionSupervisor> StartAsync(EngineLayout layout, string instanceId, string profileId,
        string? virtualSerial, string? virtualAddress, bool skipOobe, CancellationToken cancellationToken)
    {
        if (!SandboxPolicy.Available)
            throw new InvalidOperationException("缺少 /usr/bin/bwrap，无法在不降级的情况下启动会话。");
        var sessionRoot = DataLocations.NewSessionRoot();
        DataLocations.EnsurePrivateDirectory(sessionRoot);
        var rootStat = UnixNative.Stat(sessionRoot);
        var reservation = new
        {
            version = 1,
            nonce = Guid.NewGuid().ToString("N"),
            device = rootStat.Device,
            inode = rootStat.Inode,
        };
        var reservationPath = Path.Combine(sessionRoot, ".reservation.json");
        using (var handle = UnixNative.CreatePrivateExclusive(reservationPath))
            JsonSerializer.Serialize(handle, reservation);
        var supervisor = new SessionSupervisor(layout, new InspectionRunner(layout), sessionRoot);
        await supervisor.PrepareAsync(instanceId, profileId, virtualSerial, virtualAddress, skipOobe, reservation.nonce, cancellationToken);
        return supervisor;
    }

    private async Task PrepareAsync(string instanceId, string profileId, string? virtualSerial, string? virtualAddress,
        bool skipOobe, string reservationNonce, CancellationToken cancellationToken)
    {
        var arguments = new List<string>
        {
            "--profile-id", profileId, "--session-root", _sessionRoot,
            "--instance-id", instanceId, "--reservation-nonce", reservationNonce,
            skipOobe ? "--skip-oobe" : "--no-skip-oobe",
        };
        if (!string.IsNullOrEmpty(virtualSerial)) { arguments.Add("--virtual-serial"); arguments.Add(virtualSerial); }
        if (!string.IsNullOrEmpty(virtualAddress)) { arguments.Add("--virtual-address"); arguments.Add(virtualAddress); }
        var receipt = await _inspection.RunCheckedAsync("prepare-bundled", arguments,
            libraryWritable: false, readWriteTrees: new[] { _sessionRoot }, cancellationToken: cancellationToken);
        ApprovalDigest = JsonRead.AsString(receipt["approvalDigest"])
                         ?? throw new InvalidDataException("准备回执缺少 identityDigest/approvalDigest");
        IdentityDigest = JsonRead.AsString(receipt["identityDigest"]) ?? ApprovalDigest;
    }

    public string ApprovalDigest { get; private set; } = "";
    public string IdentityDigest { get; private set; } = "";

    public async Task<BridgeClient> LaunchAsync(int maxRuntimeSeconds = 0, CancellationToken cancellationToken = default)
    {
        if (_client is not null) return _client;
        var sessionId = Guid.NewGuid();
        _runName = "m-" + Guid.NewGuid().ToString("N")[..12];
        var run = Path.Combine(_sessionDirectory, _runName);
        DataLocations.EnsurePrivateDirectory(Path.Combine(_sessionDirectory, "host-tmp"));
        DataLocations.EnsurePrivateDirectory(Path.Combine(run, "tmp"));
        Directory.CreateDirectory(_brokerDirectory);
        File.SetUnixFileMode(_brokerDirectory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var config = new JsonObject
        {
            ["version"] = 1,
            ["id"] = sessionId.ToString(),
            ["run"] = _runName,
            ["approvalDigest"] = ApprovalDigest,
            ["maxRuntimeSeconds"] = maxRuntimeSeconds,
        };
        PublishExclusive(Path.Combine(_brokerDirectory, "config.json"), config.ToJsonString());

        var helper = StartProcess(BuildHelperInvocation(maxRuntimeSeconds), Path.Combine(_sessionDirectory, "host-tmp"),
            Path.Combine(_sessionDirectory, $"helper-{sessionId:N}.log"), redirectStdinNull: true);
        _helper = helper.Process;
        _helperLog = helper.Log;
        var deadline = (MonotonicClock.Now + TimeSpan.FromSeconds(20));
        while (!File.Exists(Path.Combine(_sessionDirectory, "control.sock")))
        {
            if (_helper.HasExited || MonotonicClock.Now > deadline)
                throw new IOException(_helper.HasExited ? "会话辅助进程提前退出" : "等待控制通道超时");
            await Task.Delay(25, cancellationToken);
        }
        _client = new BridgeClient(_sessionDirectory);
        _watchCancellation = new CancellationTokenSource();
        _watchTask = Task.Run(() => WatchBrokerAsync(_watchCancellation.Token));
        // The helper serves from the moment its socket appears; start is what brings
        // the core up, and a refusal here must reach the caller as a failed launch.
        _client.Command("start");
        return _client;
    }

    private (string FileName, List<string> Arguments, Dictionary<string, string> Environment, string WorkingDirectory)
        BuildHelperInvocation(int maxRuntime)
    {
        var policy = new SandboxPolicy
        {
            ReadOnlyTrees = { _layout.EngineRoot, _layout.RuntimeRoot, _layout.FirmwareRoot },
            // The whole azki-b root is writable: session (socket, logs, broker
            // files, state) plus prepared/ (the plan pins H must read and the
            // receipt it validates). Seatbelt granted the pin files read-only;
            // bwrap directory binds expose prepared as writable — acceptable
            // because H is the only process inside this namespace and the plan
            // digest is re-verified on every start.
            ReadWriteTrees = { _sessionRoot },
        };
        var args = policy.BuildArguments().ToList();
        args.Add(_layout.Python);
        args.AddRange(new[] { "-I", "-S", "-B", _layout.Cli, "serve", _sessionDirectory,
            "--max-runtime", maxRuntime.ToString(), "--approved-identity-sha256", ApprovalDigest,
            "--no-discovery", "--native-broker" });
        var environment = ScrubbedEnvironment(_sessionDirectory, Path.Combine(_sessionDirectory, "host-tmp"));
        return (SandboxPolicy.Launcher, args, environment, _sessionDirectory);
    }

    private (string FileName, List<string> Arguments, Dictionary<string, string> Environment, string WorkingDirectory)
        BuildGuestInvocation(string projectDirectory, string profileId, JsonObject backendPlan)
    {
        var run = Path.Combine(_sessionDirectory, _runName);
        var instanceData = Path.Combine(_sessionDirectory, "instance-data");
        var assets = Path.Combine(projectDirectory, "assets");
        var policy = new SandboxPolicy
        {
            ReadOnlyTrees = { _layout.EngineRoot, projectDirectory },
            ReadWriteTrees = { run },
        };
        if (Directory.Exists(instanceData)) policy.ReadWriteTrees.Add(instanceData);
        if (Directory.Exists(assets)) policy.ReadWriteTrees.Add(assets);
        var args = policy.BuildArguments().ToList();
        args.Add(_layout.Python);
        args.AddRange(new[] { "-I", "-S", "-B", _layout.MinativeLaunch, "--native", "0",
            Path.Combine(run, "runner-process.json"), Environment.ProcessId.ToString(),
            _layout.EngineRoot, projectDirectory, profileId, run });
        var storage = backendPlan["identity"]?["instanceStorage"] as JsonObject;
        if (storage is not null)
        {
            string PathOf(string key) => JsonRead.AsString(storage[key])
                ?? throw new InvalidDataException($"backend-plan 缺少 {key}");
            args.AddRange(new[]
            {
                "--instance-data-image", PathOf("dataImage"), "--instance-spare-image", PathOf("spareImage"),
                "--instance-data-sha256", PathOf("dataSHA256"), "--instance-spare-sha256", PathOf("spareSHA256"),
            });
            if (JsonRead.AsString(storage["norImage"]) is { } nor)
            {
                args.AddRange(new[] { "--instance-nor-image", nor, "--instance-nor-sha256", PathOf("norSHA256") });
                if (JsonRead.AsBool(storage["norInitialize"])) args.Add("--instance-nor-initialize");
            }
        }
        args.Add("--skip-oobe");
        var environment = ScrubbedEnvironment(run, Path.Combine(run, "tmp"));
        return (SandboxPolicy.Launcher, args, environment, projectDirectory);
    }

    private static Dictionary<string, string> ScrubbedEnvironment(string home, string tmp) => new()
    {
        ["PATH"] = "/usr/bin:/bin",
        ["LANG"] = "en_US.UTF-8",
        ["LC_ALL"] = "en_US.UTF-8",
        ["HOME"] = home,
        ["TMPDIR"] = tmp,
        ["PYTHONNOUSERSITE"] = "1",
        ["PYTHONDONTWRITEBYTECODE"] = "1",
    };

    private (Process Process, LogWriter Log) StartProcess((string FileName, List<string> Arguments, Dictionary<string, string> Environment, string WorkingDirectory) invocation,
        string workingDirectory, string logPath, bool redirectStdinNull = false)
    {
        var writer = new LogWriter(File.Open(logPath, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None));
        Directory.CreateDirectory(workingDirectory);
        var info = new ProcessStartInfo
        {
            FileName = invocation.FileName,
            WorkingDirectory = invocation.WorkingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var argument in invocation.Arguments) info.ArgumentList.Add(argument);
        foreach (var (key, value) in invocation.Environment) info.Environment[key] = value;
        info.Environment.Remove("AZKI_APP_ROOT");
        var process = Process.Start(info) ?? throw new InvalidOperationException("无法启动沙箱进程");
        if (redirectStdinNull) process.StandardInput.Close();
        writer.PumpFrom(process.StandardOutput.BaseStream);
        writer.PumpFrom(process.StandardError.BaseStream);
        return (process, writer);
    }

    /// <summary>One log file, two producers. stdout and stderr are pumped concurrently,
    /// so an unsynchronized shared FileStream would race the write position and overwrite
    /// lines; every byte is copied rather than decoded because a live tail splits UTF-8
    /// sequences across reads.</summary>
    private sealed class LogWriter : IAsyncDisposable
    {
        private readonly FileStream _stream;
        private readonly SemaphoreSlim _gate = new(1, 1);
        private int _activePumps;

        public LogWriter(FileStream stream) => _stream = stream;

        public void PumpFrom(Stream source)
        {
            Interlocked.Increment(ref _activePumps);
            _ = Task.Run(async () =>
            {
                try
                {
                    var buffer = new byte[4096];
                    int read;
                    while ((read = await source.ReadAsync(buffer.AsMemory(0, buffer.Length))) > 0)
                    {
                        await _gate.WaitAsync();
                        try { await _stream.WriteAsync(buffer.AsMemory(0, read)); }
                        finally { _gate.Release(); }
                    }
                }
                catch (IOException) { /* process death ends the tail */ }
                catch (Exception) { /* a dead log must not outlive the session claim */ }
                finally
                {
                    if (Interlocked.Decrement(ref _activePumps) == 0)
                    {
                        await _gate.WaitAsync();
                        try { await _stream.FlushAsync(); }
                        catch (IOException) { }
                        finally { _gate.Release(); }
                    }
                }
            });
        }

        public async ValueTask DisposeAsync()
        {
            await _gate.WaitAsync();
            try
            {
                await _stream.FlushAsync();
                _stream.Dispose();
            }
            catch (IOException) { }
            finally
            {
                _gate.Release();
                _gate.Dispose();
            }
        }
    }

    /// <summary>Watches native-broker requests from H: launch.json spawns G under its own sandbox; stop.json reaps it.</summary>
    private async Task WatchBrokerAsync(CancellationToken cancellationToken)
    {
        var launchPath = Path.Combine(_brokerDirectory, "launch.json");
        var stopPath = Path.Combine(_brokerDirectory, "stop.json");
        var deadline = (MonotonicClock.Now + TimeSpan.FromSeconds(120));
        try
        {
            while (!File.Exists(launchPath))
            {
                if (MonotonicClock.Now > deadline || _guest is { HasExited: true }) return;
                await Task.Delay(50, cancellationToken);
            }
            var request = JsonNode.Parse(File.ReadAllText(launchPath)) as JsonObject
                          ?? throw new InvalidDataException("launch.json 不是 JSON 对象");
            if (JsonRead.AsInt(request["version"], -1) != 1 || JsonRead.AsString(request["run"]) != _runName)
                throw new InvalidDataException("launch.json 与本次会话不符");
            var requestId = JsonRead.AsString(request["id"]) ?? throw new InvalidDataException("launch.json 缺少 id");

            var plan = JsonNode.Parse(File.ReadAllText(Path.Combine(_sessionDirectory, "backend-plan.json"))) as JsonObject
                       ?? throw new InvalidDataException("会话缺少 backend-plan.json");
            var spec = plan["spec"] as JsonObject ?? throw new InvalidDataException("backend-plan 缺少 spec");
            var projectDirectory = JsonRead.AsString(spec["project"])
                                   ?? throw new InvalidDataException("backend-plan 缺少 project");
            var profileId = JsonRead.AsString(plan["profileID"]) ?? JsonRead.AsString(spec["profile_id"]) ?? "";
            var invocation = BuildGuestInvocation(projectDirectory, profileId, plan);
            var guest = StartProcess(invocation, invocation.WorkingDirectory,
                Path.Combine(_sessionDirectory, _runName, "adapter-runner.log"));
            _guest = guest.Process;
            _guestLog = guest.Log;

            var setsid = (MonotonicClock.Now + TimeSpan.FromSeconds(1));
            while (UnixNative.getpgid(_guest.Id) != _guest.Id)
            {
                if (_guest.HasExited) throw new IOException("客户监督进程未能建立自己的进程组");
                if (MonotonicClock.Now > setsid) throw new IOException("客户监督进程建立进程组超时");
                await Task.Delay(10, cancellationToken);
            }
            _guest.StandardInput.Write("AZKI-GUEST-GO\n");
            _guest.StandardInput.Flush();
            _guest.StandardInput.Close();
            PublishState(new JsonObject { ["version"] = 1, ["id"] = requestId,
                ["run"] = _runName, ["state"] = "running", ["pid"] = _guest.Id });

            while (true)
            {
                if (File.Exists(stopPath))
                {
                    StopGuest();
                    var exit = _guest.WaitForExit(13_000) ? _guest.ExitCode : 137;
                    PublishState(new JsonObject { ["version"] = 1, ["id"] = requestId,
                        ["run"] = _runName, ["state"] = exit == 0 ? "exited" : "failed",
                        ["exitCode"] = exit, ["cleanup"] = "reaped", ["pid"] = _guest.Id });
                    _publishedTerminal = true;
                    return;
                }
                if (_guest.HasExited)
                {
                    var exit = _guest.ExitCode;
                    PublishState(new JsonObject { ["version"] = 1, ["id"] = requestId,
                        ["run"] = _runName, ["state"] = exit == 0 ? "exited" : "failed",
                        ["exitCode"] = exit, ["cleanup"] = CleanupConfirmed() ? "reaped" : "unconfirmed", ["pid"] = _guest.Id });
                    _publishedTerminal = true;
                    return;
                }
                await Task.Delay(50, cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
            StopGuest();
        }
        catch (Exception error)
        {
            if (!_publishedTerminal)
            {
                try
                {
                    PublishState(new JsonObject { ["version"] = 1, ["id"] = _brokerSessionId, ["run"] = _runName,
                        ["state"] = "failed", ["exitCode"] = -1, ["cleanup"] = CleanupConfirmed() ? "reaped" : "unconfirmed",
                        ["error"] = error.Message });
                }
                catch (IOException) { }
            }
        }
    }

    private string _brokerSessionId = "";

    private bool CleanupConfirmed()
    {
        var group = _guest?.Id ?? 0;
        if (group == 0) return true;
        // /proc is read directly rather than through Process.GetProcesses plus getpgid:
        // another user's pid answers EPERM, which would make every shared machine report
        // "unconfirmed", and a pid that vanishes mid-scan is not evidence of a survivor.
        foreach (var entry in Directory.EnumerateDirectories("/proc"))
        {
            var name = Path.GetFileName(entry);
            if (name.Length == 0 || !name.All(char.IsAsciiDigit) || !int.TryParse(name, out var pid) || pid == group)
                continue;
            try
            {
                var text = File.ReadAllText(Path.Combine(entry, "stat"));
                var close = text.LastIndexOf(')');
                if (close < 1 || text.Length <= close + 2) continue;
                // After the parenthesised command: state, ppid, pgrp.
                var fields = text.Substring(close + 2).Split(' ');
                if (fields.Length > 2 && int.TryParse(fields[2], out var pgrp) && pgrp == group) return false;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                continue;
            }
        }
        return true;
    }

    private void StopGuest()
    {
        if (_guest is null || _guest.HasExited) return;
        try
        {
            UnixNative.killpg(_guest.Id, 15); // SIGTERM to the owned group
            if (!_guest.WaitForExit(8_000)) UnixNative.killpg(_guest.Id, 9);
            _guest.WaitForExit(2_000);
        }
        catch (Exception) { /* group may already be gone */ }
    }

    private void PublishState(JsonObject state)
    {
        _brokerSessionId = state["id"]?.GetValue<string>() ?? _brokerSessionId;
        var path = Path.Combine(_brokerDirectory, "state.json");
        var temporary = Path.Combine(_brokerDirectory, $".state-{Guid.NewGuid():N}.tmp");
        File.WriteAllText(temporary, state.ToJsonString(new JsonSerializerOptions { WriteIndented = false }));
        File.SetUnixFileMode(temporary, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        if (!File.Exists(path))
        {
            UnixNative.TryHardLink(temporary, path, out _);
        }
        else
        {
            File.Move(temporary, path, overwrite: true);
        }
        try { File.Delete(temporary); } catch (IOException) { }
    }

    private static void PublishExclusive(string path, string content)
    {
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, content);
        File.SetUnixFileMode(temporary, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        if (!UnixNative.TryHardLink(temporary, path, out var error))
            throw new IOException($"原子发布失败 (errno {error})");
        File.Delete(temporary);
    }

    public async ValueTask DisposeAsync()
    {
        _watchCancellation?.Cancel();
        if (_watchTask is not null)
        {
            try { await _watchTask; } catch (Exception) { }
        }
        _client?.Dispose();
        StopGuest();
        if (_helper is { HasExited: false })
        {
            try { _helper.Kill(entireProcessTree: false); } catch (InvalidOperationException) { }
            try { _helper.WaitForExit(3_000); } catch { }
        }
        var guestLog = _guestLog;
        if (guestLog is not null) await guestLog.DisposeAsync();
        var helperLog = _helperLog;
        if (helperLog is not null) await helperLog.DisposeAsync();
        _guestLog = null;
        _helperLog = null;
        _helper?.Dispose();
        _guest?.Dispose();
    }
}
