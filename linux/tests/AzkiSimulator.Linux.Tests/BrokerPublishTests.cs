using System.Diagnostics;
using System.IO;
using System.Text.Json;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// Broker records are read by the sandboxed helper, which refuses any file whose
/// link count is not exactly one. Publishing by hard link plus unlink therefore
/// opens a window in which the helper's 25 ms poll rejects its own status record,
/// so publication goes through renameat2(RENAME_NOREPLACE) like the helper's own
/// writer and the macOS reference do.
/// </summary>
public class BrokerPublishTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"azki-broker-{Guid.NewGuid():N}");

    public BrokerPublishTests()
    {
        Directory.CreateDirectory(_root);
        DataLocations.EnsurePrivateDirectory(_root);
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);

    private string At(string name) => Path.Combine(_root, name);

    [Fact]
    public void ExclusivePublishLeavesExactlyOneLinkAndNoTemporary()
    {
        var target = At("state.json");
        using (var stream = UnixNative.CreatePrivateExclusive(At("staging")))
        {
            stream.Write("{ \"state\": \"running\" }"u8);
        }
        UnixNative.RenameExclusive(At("staging"), target);

        Assert.False(File.Exists(At("staging")));
        // The helper's own reader is the oracle: one open enforces regular type,
        // ownership, a link count of exactly one, and the size bound.
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            using var record = UnixNative.OpenRegularFile(directory, "state.json", 4096);
            Assert.NotNull(record);
        }
        Assert.Empty(Directory.GetFiles(_root, ".*", SearchOption.TopDirectoryOnly));
    }

    [Fact]
    public void ASecondLinkIsWhatTheReaderRefuses()
    {
        // The old publish path (link plus unlink) put the record in exactly this
        // shape for as long as the temporary name survived.
        var target = At("state.json");
        using (var stream = UnixNative.CreatePrivateExclusive(At("staging")))
        {
            stream.Write("running"u8);
        }
        UnixNative.HardLink(At("staging"), target);
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            Assert.Throws<IOException>(() => UnixNative.OpenRegularFile(directory, "state.json", 4096));
        }
    }

    [Fact]
    public void ExclusivePublishRefusesToReplaceAnExistingRecord()
    {
        var target = At("config.json");
        File.WriteAllText(target, "first");
        using (var stream = UnixNative.CreatePrivateExclusive(At("staging")))
        {
            stream.Write("second"u8);
        }
        var failure = Assert.Throws<IOException>(() => UnixNative.RenameExclusive(At("staging"), target));
        Assert.Contains("17", failure.Message); // EEXIST
        Assert.Equal("first", File.ReadAllText(target));
    }

    [Fact]
    public void RecordReaderFollowsTheSameRulesAsTheWriter()
    {
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            Assert.Null(UnixNative.ReadRecord(directory, "launch.json", 4096)); // not published yet
            WriteRecord("launch.json", """{"version":1,"id":"x","run":"m-0123456789ab"}""");
            Assert.Contains("\"version\":1", UnixNative.ReadRecord(directory, "launch.json", 4096));

            // A symlink inside the broker directory must not be read through: the
            // watcher is about to start a sandboxed process from what it parses.
            Directory.CreateSymbolicLink(At("alias.json"), At("launch.json"));
            Assert.Throws<IOException>(() => UnixNative.ReadRecord(directory, "alias.json", 4096));

            Assert.Throws<IOException>(() =>
            {
                WriteRecord("group-writable.json", "{}");
                File.SetUnixFileMode(At("group-writable.json"), UnixFileMode.UserRead | UnixFileMode.UserWrite
                    | UnixFileMode.GroupWrite);
                UnixNative.ReadRecord(directory, "group-writable.json", 4096);
            });

            // The bound is enforced before the read, so an oversized record is
            // refused rather than streamed into the watcher.
            WriteRecord("big.json", new string('x', 5000));
            Assert.Throws<IOException>(() => UnixNative.ReadRecord(directory, "big.json", 4096));
        }
    }

    [Fact]
    public void PrivateCreationIsPrivateFromItsFirstByte()
    {
        var path = At("mode-check");
        using (var stream = UnixNative.CreatePrivateExclusive(path))
        {
            // The mode has to be right before a single byte is written: create
            // then chmod would leave a readable window under the default umask.
            Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(path));
            stream.Write("payload"u8);
        }
        Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(path));
    }

    /// <summary>
    /// The two halves of the broker handshake are written by different languages,
    /// so each direction is checked against the other side's real reader: the
    /// helper publishes launch.json with renameat2(RENAME_NOREPLACE) and the
    /// supervisor answers with the same idiom, read back by the helper's own
    /// "bounded, owned, non-linked regular file" validator.
    /// </summary>
    [Fact]
    public async Task BothSidesReadWhatTheOtherSidePublishes()
    {
        var runtime = RepositoryRuntime();
        var python = Environment.GetEnvironmentVariable("AZKI_PYTHON") ?? "/usr/bin/python3";
        if (runtime is null || !File.Exists(python)) return; // no source tree or interpreter: nothing to pair

        var sessionId = Guid.NewGuid().ToString("D");
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            var published = await RunAsync(python,
                $$"""
                 import os, sys
                 sys.path.insert(0, {{JsonSerializer.Serialize(runtime)}})
                 from azki_simulator import native_broker
                 fd = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                 native_broker._publish(fd, "launch.json", {"version": 1, "id": sys.argv[2], "run": sys.argv[3]})
                 """,
                _root, sessionId, "m-0123456789ab");
            Assert.True(published.ExitCode == 0, $"helper 发布失败: {published.Stderr}");

            var launch = UnixNative.ReadRecord(directory, "launch.json", 4096);
            Assert.NotNull(launch);
            Assert.Contains("\"version\":1", launch);
            Assert.Contains(sessionId, launch);
        }

        // Now the supervisor's own record, read by the helper's reader.
        var state = At("state.json");
        using (var stream = UnixNative.CreatePrivateExclusive(state))
        {
            stream.Write("""{"version":1,"id":"a","run":"m-0123456789ab","state":"running","pid":4242}"""u8);
        }
        UnixNative.RenameExclusive(state, At("handshake.json"));
        var read = await RunAsync(python,
            $$"""
             import sys
             from pathlib import Path
             sys.path.insert(0, {{JsonSerializer.Serialize(runtime)}})
             from azki_simulator.native_broker import _record
             value, _ = _record(Path(sys.argv[1]))
             print(value["state"])
             """,
            At("handshake.json"));
        Assert.True(read.ExitCode == 0, $"helper 读取失败: {read.Stderr}");
        Assert.Equal("running", read.Stdout.Trim());
    }

    private static string? RepositoryRuntime()
    {
        for (var probe = new DirectoryInfo(AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, "Sources/AzkiSimulator/Runtime");
            if (File.Exists(Path.Combine(candidate, "azki_simulator/native_broker.py"))) return candidate;
        }
        return null;
    }

    private static async Task<(int ExitCode, string Stdout, string Stderr)> RunAsync(string file, string arguments,
        params string[] rest)
    {
        var startInfo = new ProcessStartInfo(file) { RedirectStandardOutput = true, RedirectStandardError = true };
        startInfo.ArgumentList.Add("-I");
        startInfo.ArgumentList.Add("-S");
        startInfo.ArgumentList.Add("-B");
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(arguments);
        foreach (var value in rest) startInfo.ArgumentList.Add(value);
        using var process = Process.Start(startInfo)!;
        var stdout = await process.StandardOutput.ReadToEndAsync();
        var stderr = await process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, stdout, stderr);
    }

    private void WriteRecord(string name, string content)
    {
        using var stream = UnixNative.CreatePrivateExclusive(At(name));
        stream.Write(System.Text.Encoding.UTF8.GetBytes(content));
    }
}
