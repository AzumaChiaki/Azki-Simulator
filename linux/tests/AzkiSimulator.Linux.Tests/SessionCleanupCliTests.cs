using System.Diagnostics;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The cleanup button's whole chain on a real machine: the sandboxed pass sees exactly the
/// stale roots the grant table advertises, empties them, cannot unlink the bound mount
/// point, and the host finishes the job. Without the second half this is the failure mode
/// nobody notices — the disk is reclaimed but an empty directory is left behind per
/// session forever, and the receipt calls it a removal failure.
/// Runs only with AZKI_APP_ROOT at an extracted package; CI excludes it by category.
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class SessionCleanupCliTests
{
    private static int DeadPid()
    {
        using var child = Process.Start("/bin/true") ?? throw new InvalidOperationException("无法启动探针进程");
        child.WaitForExit();
        return child.Id;
    }

    private static string MakeRoot(string tmpRoot, string name, int pid, bool payload = false)
    {
        var path = Path.Combine(tmpRoot, name);
        Directory.CreateDirectory(Path.Combine(path, "session"));
        File.WriteAllText(Path.Combine(path, "runner-process.json"), $"{{\"pid\": {pid}}}");
        if (payload) File.WriteAllText(Path.Combine(path, "session", "holder.bin"), new string('x', 4096));
        return path;
    }

    [Fact]
    [Trait("Category", "Integration")]
    public async Task EmptiesStaleRootsInSandboxAndReclaimsThemOnTheHost()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        var tmpRoot = Path.Combine(Path.GetTempPath(), $"azki-tmp-{Guid.NewGuid():N}");
        var dataRoot = Path.Combine(Path.GetTempPath(), $"azki-cli-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tmpRoot);
        var previousTmp = Environment.GetEnvironmentVariable("AZKI_TMP_ROOT");
        var previousData = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", tmpRoot);
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", dataRoot);
        var live = Process.Start("/bin/sleep", "30") ?? throw new InvalidOperationException("无法启动存活探针");
        try
        {
            var stale = MakeRoot(tmpRoot, "azki-b-stale-" + Guid.NewGuid().ToString("N")[..8], DeadPid(), payload: true);
            var running = MakeRoot(tmpRoot, "azki-b-running", live.Id);
            var foreign = MakeRoot(tmpRoot, "not-azki-keepme", DeadPid(), payload: true);
            var preparing = Path.Combine(tmpRoot, "azki-sim-preparing");
            Directory.CreateDirectory(preparing);

            var candidates = CliGrants.StaleSessionRoots().ToList();
            Assert.Contains(stale, candidates);
            Assert.Contains(running, candidates);
            Assert.Contains(preparing, candidates);
            Assert.DoesNotContain(foreign, candidates);

            var receipt = await new InspectionRunner(layout!).RunCheckedAsync(
                "sessions-cleanup", new[] { "--tmp-root", tmpRoot });

            // Inside the sandbox the tree is gone but the mount point is not, and the
            // receipt says so with the kernel's own reason.
            Assert.True(Directory.Exists(stale));
            Assert.Empty(Directory.GetFileSystemEntries(stale));
            Assert.True(File.Exists(Path.Combine(running, "runner-process.json")), "运行中的会话被动了");
            Assert.True(File.Exists(Path.Combine(foreign, "session", "holder.bin")), "前缀之外的目录被清理了");
            var reasons = (JsonObject?)receipt["reasons"] ?? throw new InvalidDataException("清理回执没有 reasons");
            Assert.StartsWith("remove-failed", JsonRead.AsString(reasons[stale]) ?? "");
            Assert.Equal("running", JsonRead.AsString(reasons[running]));
            Assert.Equal("recent", JsonRead.AsString(reasons[preparing]));

            var reclaimed = SessionCleanup.Reclaim(receipt, candidates);
            Assert.Equal([stale], reclaimed);
            Assert.False(Directory.Exists(stale));
            Assert.True(Directory.Exists(running));
            Assert.True(Directory.Exists(foreign));
            // The empty-but-recent root stays: the sandbox kept it on purpose, and it may
            // belong to a session being prepared in another window right now.
            Assert.True(Directory.Exists(preparing));
        }
        finally
        {
            try { live.Kill(entireProcessTree: true); live.WaitForExit(); } catch (InvalidOperationException) { }
            live.Dispose();
            Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", previousTmp);
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", previousData);
            try { Directory.Delete(tmpRoot, recursive: true); } catch (IOException) { }
            try { Directory.Delete(dataRoot, recursive: true); } catch (IOException) { }
        }
    }
}
