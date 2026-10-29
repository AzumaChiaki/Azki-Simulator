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
    // The supervisor inside the guest sandbox, not the bwrap launcher: its process
    // group is what gets published as the session PID and what has to be reaped.
    private volatile int _guestGroup;
    private LogWriter? _helperLog;
    // Written by the broker watcher, read by teardown on another thread.
    private volatile LogWriter? _guestLog;
    private BridgeClient? _client;
    private CancellationTokenSource? _watchCancellation;
    private Task? _watchTask;
    private string _runName = "";
    private string _sessionId = "";
    // Written by the launch, read by the broker watcher on its own thread.
    private volatile int _maxRuntime;
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
            cancellationToken: cancellationToken);
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
        _sessionId = sessionId.ToString();
        _maxRuntime = maxRuntimeSeconds;
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
        UnixNative.PublishRecord(_brokerDirectory, "config", "config.json", config.ToJsonString(), mayReplace: false);

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
            ReadOnlyTrees = { _layout.EngineRoot, _layout.RuntimeRoot, _layout.FirmwareRoot,
                // start revalidates every path the plan pins, and an imported
                // package's AP/OTA objects live in the private library. Without
                // this the helper failed on the first command it was asked to run.
                DataLocations.FirmwareLibraryRoot },
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
            ReadOnlyTrees = { _layout.EngineRoot, projectDirectory,
                // The runner opens the AP and resource images the prepared catalog
                // names, and for an imported package those are library objects.
                DataLocations.FirmwareLibraryRoot },
            // bwrap's root is an empty tmpfs, so a script the argv names has to be
            // bound or the interpreter reports ENOENT for it. Only the supervisor
            // itself, not the whole Runtime tree: the payload it execs is Engine's
            // bootstrap.py plus the project's runner, and the reference guest reads
            // the supervisor as a pinned file rather than as part of a tree.
            ReadOnlyFiles = { _layout.MinativeLaunch },
            ReadWriteTrees = { run },
        };
        if (Directory.Exists(instanceData)) policy.ReadWriteTrees.Add(instanceData);
        if (Directory.Exists(assets)) policy.ReadWriteTrees.Add(assets);
        var args = policy.BuildArguments().ToList();
        args.Add(_layout.Python);
        args.AddRange(new[] { "-I", "-S", "-B", _layout.MinativeLaunch, "--native", _maxRuntime.ToString(),
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
            // The synthetic peer is a plan-bound input: the state file and its digest
            // come from the same snapshot the guest's identity is checked against, and
            // the pending copy is written inside this run. Order matters here -- the
            // guest supervisor validates the exact argv shape, including which argument
            // positions the peer may occupy.
            if (JsonRead.AsBool(storage["virtualPeerEnabled"]))
            {
                args.AddRange(new[]
                {
                    "--virtual-peer-state", PathOf("virtualPeerStatePath"),
                    "--virtual-peer-state-sha256", PathOf("virtualPeerStateSHA256"),
                    "--virtual-peer-output", Path.Combine(run, "virtual-peer-state.pending.json"),
                });
            }
            // Host-authored firmware settings, applied by the runner before boot.
            if (JsonRead.AsString(storage["settingsPath"]) is { } settings)
                args.AddRange(new[] { "--instance-settings", settings });
        }
        // The plan owns the OOBE choice, so it is read back rather than re-decided:
        // only an explicit false asks the guest to run setup.
        args.Add(backendPlan["skipOOBE"]?.GetValueKind() is JsonValueKind.False ? "--no-skip-oobe" : "--skip-oobe");
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
        // 0600, never the umask default: the helper refuses any run output that is
        // world-readable or a symlink, and it inspects the guest log the app itself
        // creates -- exactly the rule Swift's O_CREAT|O_EXCL ... 0o600 open enforces.
        var writer = new LogWriter(UnixNative.CreatePrivateExclusive(logPath));
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
        // Both requests are read through one validated directory handle, bounded
        // and identity-checked, exactly like the helper's own record reader: the
        // watcher is about to start a sandboxed process from what it parses.
        using var broker = UnixNative.OpenDirectory(_brokerDirectory);
        var deadline = (MonotonicClock.Now + TimeSpan.FromSeconds(120));
        try
        {
            string? launch;
            while ((launch = UnixNative.ReadRecord(broker, "launch.json", 4096)) is null)
            {
                if (MonotonicClock.Now > deadline || _guest is { HasExited: true }) return;
                await Task.Delay(50, cancellationToken);
            }
            var request = JsonNode.Parse(launch) as JsonObject
                          ?? throw new InvalidDataException("launch.json 不是 JSON 对象");
            // The reference compares the request against the configuration it sealed,
            // so the identity this app wrote into config.json is what must come back:
            // an id from another session would be published as this session's status.
            if (JsonRead.AsInt(request["version"], -1) != 1 || SetOfKeys(request) != "id,run,version"
                || JsonRead.AsString(request["run"]) != _runName
                || JsonRead.AsString(request["id"]) != _sessionId)
                throw new InvalidDataException("launch.json 与本次会话不符");

            // The plan drives the guest's mount arguments, so it is read under the
            // same guard as the broker requests: no symlink, bounded, unchanged.
            using var session = UnixNative.OpenDirectory(_sessionDirectory);
            var planText = UnixNative.ReadRecord(session, "backend-plan.json", 4 << 20)
                           ?? throw new InvalidDataException("会话缺少 backend-plan.json");
            var plan = JsonNode.Parse(planText) as JsonObject
                       ?? throw new InvalidDataException("backend-plan.json 不是 JSON 对象");
            var spec = plan["spec"] as JsonObject ?? throw new InvalidDataException("backend-plan 缺少 spec");
            var projectDirectory = JsonRead.AsString(spec["project"])
                                   ?? throw new InvalidDataException("backend-plan 缺少 project");
            var profileId = JsonRead.AsString(plan["profileID"]) ?? JsonRead.AsString(spec["profile_id"]) ?? "";
            var invocation = BuildGuestInvocation(projectDirectory, profileId, plan);
            var guest = StartProcess(invocation, invocation.WorkingDirectory,
                Path.Combine(_sessionDirectory, _runName, "adapter-runner.log"));
            _guest = guest.Process;
            _guestLog = guest.Log;
            _guestGroup = await WaitForGuestGroupAsync(cancellationToken);
            _guest.StandardInput.Write("AZKI-GUEST-GO\n");
            _guest.StandardInput.Flush();
            _guest.StandardInput.Close();
            PublishState(new JsonObject { ["version"] = 1, ["id"] = _sessionId,
                ["run"] = _runName, ["state"] = "running", ["pid"] = _guestGroup });

            while (true)
            {
                if (UnixNative.ReadRecord(broker, "stop.json", 4096) is { } stop)
                {
                    // The reference validates the stop request before it reaps: an
                    // unrelated or stale record must not tear down this session's guest.
                    var record = JsonNode.Parse(stop) as JsonObject
                                 ?? throw new InvalidDataException("stop.json 不是 JSON 对象");
                    if (JsonRead.AsInt(record["version"], -1) != 1 || JsonRead.AsString(record["id"]) != _sessionId
                        || SetOfKeys(record) != "id,version")
                        throw new InvalidDataException("stop.json 不是本次会话的停止请求");
                    StopGuest();
                    PublishTerminal(_guest);
                    return;
                }
                if (_guest.HasExited)
                {
                    // A supervisor that left on its own still owns the group the runner
                    // and the emulator are in, so only the stop path can confirm the reap.
                    StopGuest();
                    PublishTerminal(_guest);
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
            if (_publishedTerminal) return;
            // Reap first, exactly like the reference broker: asking whether the
            // group is gone without stopping it would publish "unconfirmed" over
            // a live guest, and that guest owns a QEMU child.
            StopGuest();
            // The record carries a fixed sentence. Exception text belongs in the
            // fault log -- it has no length bound, and the helper rejects any
            // terminal status whose error field exceeds its 1024-byte cap, which
            // would turn one failure into a protocol error. The id comes from the
            // sealed configuration rather than from a record that may never have
            // arrived, so a failure this early still reads as a real terminal state.
            UiFault.Report(error);
            try
            {
                PublishState(new JsonObject { ["version"] = 1, ["id"] = _sessionId, ["run"] = _runName,
                    ["state"] = "failed", ["exitCode"] = -1, ["cleanup"] = CleanupConfirmed() ? "reaped" : "unconfirmed",
                    ["error"] = "原生代理拒绝无效或已改变的请求" });
            }
            catch (IOException) { }
        }
    }

    /// <summary>The terminal record, in the reference's shape: the verdict is whether the
    /// owned group was reaped, and what the guest thought of itself travels as exitCode.
    /// bwrap exits with its payload's status, so the launcher is the right place to read
    /// that from once the group is gone.</summary>
    private void PublishTerminal(Process launcher)
    {
        var reaped = CleanupConfirmed();
        PublishState(new JsonObject { ["version"] = 1, ["id"] = _sessionId,
            ["run"] = _runName, ["state"] = reaped ? "exited" : "failed",
            ["exitCode"] = launcher.HasExited ? launcher.ExitCode : -1,
            ["cleanup"] = reaped ? "reaped" : "unconfirmed", ["pid"] = _guestGroup });
        _publishedTerminal = true;
    }

    private bool CleanupConfirmed()
    {
        var group = _guestGroup;
        if (group == 0) return true;
        return !ProcessGroupSurvivors.Any(group);
    }

    /// <summary>Waits for the guest supervisor to become the process .NET can see the
    /// sandbox through. The payload bwrap forks is its own process-group leader because
    /// --new-session calls setsid before the exec, and that group is the one the runner
    /// and QEMU share.</summary>
    private async Task<int> WaitForGuestGroupAsync(CancellationToken cancellationToken)
    {
        var launcher = _guest ?? throw new InvalidOperationException("客户沙箱尚未启动");
        var deadline = MonotonicClock.Now + TimeSpan.FromSeconds(3);
        while (true)
        {
            var group = ProcessGroupSurvivors.GroupLeadingChild(launcher.Id);
            if (group > 0) return group;
            if (group < 0) throw new IOException("客户沙箱里出现了多个进程组领导者");
            if (launcher.HasExited) throw new IOException("客户监督进程在交出进程组前退出");
            if (MonotonicClock.Now > deadline) throw new IOException("等待客户监督进程组超时");
            await Task.Delay(10, cancellationToken);
        }
    }

    /// <summary>Stops the guest through the group its supervisor owns. The launcher is
    /// deliberately not signalled: killing bwrap reaps the supervisor with SIGKILL through
    /// --die-with-parent, which skips the supervisor's bounded graceful stop, its storage
    /// receipt, and leaves QEMU alive as an orphan in the supervisor's group.</summary>
    private void StopGuest()
    {
        var guest = _guest;
        if (guest is null) return;
        var group = _guestGroup;
        if (group == 0)
        {
            // Nothing was ever shown to be inside the sandbox, so the launcher is all
            // there is to stop.
            if (guest.HasExited) return;
            try { guest.Kill(entireProcessTree: false); } catch (InvalidOperationException) { }
            return;
        }
        try
        {
            UnixNative.killpg(group, 15); // SIGTERM to the owned group
            if (!GroupGone(guest, group, 13_000))
            {
                UnixNative.killpg(group, 9);
                GroupGone(guest, group, 2_000);
            }
        }
        catch (Exception) { /* group may already be gone */ }
    }

    private static bool GroupGone(Process launcher, int group, int milliseconds)
    {
        var deadline = MonotonicClock.Now + TimeSpan.FromMilliseconds(milliseconds);
        while (true)
        {
            // The launcher exits only once its payload is reaped, so both halves are
            // needed: an exited launcher with a member left behind is an orphaned QEMU.
            if (launcher.HasExited && !ProcessGroupSurvivors.Any(group)) return true;
            if (MonotonicClock.Now > deadline) return false;
            Thread.Sleep(50);
        }
    }

    /// <summary>Exact field-set check: the reference refuses a broker record that
    /// carries anything beyond the agreed keys, so extra fields cannot be ignored.</summary>
    private static string SetOfKeys(JsonObject value) => string.Join(",", value.Select(pair => pair.Key).OrderBy(key => key, StringComparer.Ordinal));

    private bool _statePublished;

    private void PublishState(JsonObject state)
    {
        // The running record is published exclusively; only a later terminal record
        // may replace it. Publication goes through rename, never a hard link, because
        // the helper refuses a status file whose link count is not one.
        UnixNative.PublishRecord(_brokerDirectory, "state", "state.json",
            state.ToJsonString(new JsonSerializerOptions { WriteIndented = false }),
            mayReplace: _statePublished);
        _statePublished = true;
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
