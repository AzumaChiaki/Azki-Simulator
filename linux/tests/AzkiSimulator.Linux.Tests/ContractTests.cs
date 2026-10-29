using System.Buffers.Binary;
using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.Services;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

public class FrameContractTests
{
    private static byte[] BuildHeader(uint version, uint sequence, int width, int height, int stride, uint format = 1, string magic = "Q66FRAME")
    {
        var bytes = new byte[RawFrame.HeaderBytes];
        System.Text.Encoding.ASCII.GetBytes(magic).CopyTo(bytes, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), version);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), sequence);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), (uint)width);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20), (uint)height);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(24), (uint)stride);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(28), format);
        return bytes;
    }

    private static byte[] BuildFrame(uint sequence, int width, int height)
    {
        var full = new byte[RawFrame.HeaderBytes + width * 2 * height];
        BuildHeader(1, sequence, width, height, width * 2).CopyTo(full, 0);
        return full;
    }

    [Fact]
    public void DecodesValidFrame()
    {
        var frame = RawFrame.Decode(BuildFrame(4, 212, 520), 212, 520);
        Assert.NotNull(frame);
        Assert.Equal(4u, frame!.Sequence);
        Assert.Equal(212 * 2 * 520, frame.Pixels.Length);
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(7u)] // odd sequence: producer is mid-write
    public void OddOrZeroSequenceMeansBeingWritten(uint sequence)
    {
        Assert.Null(RawFrame.DecodeHeader(BuildHeader(1, sequence, 212, 520, 424), 212, 520));
    }

    [Fact]
    public void RejectsForeignMagicOrVersion()
    {
        Assert.Throws<InvalidDataException>(() =>
            RawFrame.DecodeHeader(BuildHeader(1, 2, 212, 520, 424, magic: "OTHERFRM"), 212, 520));
        Assert.Throws<InvalidDataException>(() => RawFrame.DecodeHeader(BuildHeader(2, 2, 212, 520, 424), 212, 520));
    }

    [Fact]
    public void RejectsGeometryMismatch()
    {
        Assert.Throws<InvalidDataException>(() => RawFrame.DecodeHeader(BuildHeader(1, 2, 192, 490, 384), 212, 520));
        Assert.Throws<InvalidDataException>(() => RawFrame.DecodeHeader(BuildHeader(1, 2, 212, 520, 999), 212, 520));
    }
}

public class SandboxPolicyTests
{
    [Fact]
    public void ArgumentOrderPutsFlagsThenBindsThenSeparator()
    {
        var policy = new SandboxPolicy
        {
            ReadOnlyTrees = { "/app/Engine" },
            ReadOnlyFiles = { "/tmp/ota.bin" },
            ReadWriteTrees = { "/tmp/azki-b-abc123def456" },
        };
        var args = policy.BuildArguments();
        // The launcher is ProcessStartInfo.FileName, never argv[0]: bwrap would exec it
        // as the sandboxed program.
        Assert.DoesNotContain(SandboxPolicy.Launcher, args);
        Assert.Equal("--die-with-parent", args[0]);
        Assert.Contains("--unshare-user", args);
        Assert.Contains("--unshare-net", args);
        // A private pid namespace would hide the guest supervisor's real pid and
        // process group from the host, so it must never be requested.
        Assert.DoesNotContain("--unshare-pid", args);
        Assert.DoesNotContain("--unshare-all", args);
        Assert.DoesNotContain("--share-pid", args);
        // bwrap's root is empty: without the usr-merge links, exec of any binary
        // under the bound /usr fails on its /lib64 interpreter path.
        Assert.True(ContainsRun(args, "--symlink", "usr/lib64", "/lib64"));
        Assert.True(ContainsRun(args, "--symlink", "usr/bin", "/bin"));
        Assert.Contains("--die-with-parent", args);
        Assert.Equal("--", args[^1]);
        var separator = args.IndexOf("--");
        var head = args.Take(separator).ToList();
        Assert.Contains("--ro-bind", head);
        Assert.Contains("--bind", head);
        // The fixed preamble already ro-binds /usr and /etc, so a policy tree is
        // only correct when its flag and both mount points stay consecutive.
        Assert.True(ContainsRun(args, "--ro-bind", "/app/Engine", "/app/Engine"));
        Assert.True(ContainsRun(args, "--ro-bind", "/tmp/ota.bin", "/tmp/ota.bin"));
        Assert.True(ContainsRun(args, "--bind", "/tmp/azki-b-abc123def456", "/tmp/azki-b-abc123def456"));
    }

    private static bool ContainsRun(List<string> args, params string[] run)
    {
        for (var start = 0; start + run.Length <= args.Count; start++)
        {
            if (Enumerable.Range(0, run.Length).Any(offset => args[start + offset] != run[offset])) continue;
            return true;
        }
        return false;
    }

    [Fact]
    public void DeduplicatesTrees()
    {
        var policy = new SandboxPolicy { ReadOnlyTrees = { "/a", "/a" } };
        Assert.Equal(2, policy.BuildArguments().Count(argument => argument == "/a"));
    }
}

public class BridgeEnvelopeTests
{
    [Fact]
    public void ManifestDecodeRequiresV1FixedNames()
    {
        var json = new JsonObject
        {
            ["version"] = 1, ["id"] = "0f6a1d3c-1111-2222-3333-444455556666",
            ["socket"] = "control.sock", ["framebuffer"] = "frame.raw",
            ["width"] = 212, ["height"] = 520, ["backend"] = "minative",
            ["profileID"] = "q66@4.100.177@aabbccddee11", ["approvalDigest"] = new string('a', 64),
        }.ToJsonString();
        var manifest = SessionManifest.Decode(json);
        Assert.Equal(212, manifest.Width);
        Assert.Equal("minative", manifest.Backend);
        Assert.Equal(new string('a', 64), manifest.ApprovalDigest);
    }

    [Fact]
    public void ManifestDecodeRejectsUnexpectedSocketName()
    {
        Assert.Throws<InvalidDataException>(() => SessionManifest.Decode(
            new JsonObject { ["version"] = 1, ["id"] = "x", ["socket"] = "other.sock",
                             ["framebuffer"] = "frame.raw", ["width"] = 1, ["height"] = 1 }.ToJsonString()));
    }
}

public class PmuAdcTests
{    [Fact]
    public void ValuesRequireEightIntegersInRange()
    {
        var response = new JsonObject { ["state"] = new JsonObject { ["values"] = new JsonArray(
            Enumerable.Range(0, 8).Select(index => (JsonNode)(index * 1000)).ToArray()) } };
        Assert.Equal(8, PmuAdc.Values(response)!.Length);
        var tooBig = new JsonObject { ["state"] = new JsonObject { ["values"] = new JsonArray(
            Enumerable.Range(0, 8).Select(_ => (JsonNode)70_000).ToArray()) } };
        Assert.Null(PmuAdc.Values(tooBig));
        // The bridge delivers wire JSON, whose numbers are JsonElement-backed.
        var wire = JsonNode.Parse("""{"state":{"values":[0,1000,2000,3000,4000,5000,6000,7000]}}""");
        Assert.Equal(8, PmuAdc.Values(wire)!.Length);
    }

    [Fact]
    public void DraftKeepsUserEditsAgainstRefresh()
    {
        var draft = new PmuAdcDraft();
        draft.Receive(new JsonObject { ["state"] = new JsonObject { ["values"] = new JsonArray(
            Enumerable.Range(0, 8).Select(index => (JsonNode)21_000).ToArray()) } }.AsObject());
        Assert.All(draft.Text, text => Assert.Equal("21000", text));
        draft.Text[3] = "21500";
        Assert.True(draft.HasChanges);
        Assert.True(draft.CanAdopt(0, "21000"));
        Assert.False(draft.CanAdopt(3, "21500"));
    }
}

public class PosixOpenTests
{
    [Fact]
    public void FlagConstantsMatchGlibc()
    {
        // Octal spellings from <fcntl.h>: a wrong hex translation silently turns into
        // another flag (0x40000 is O_NOATIME), and the protection just disappears.
        Assert.Equal(0x10000, UnixNative.O_DIRECTORY);  // 0200000
        Assert.Equal(0x20000, UnixNative.O_NOFOLLOW);   // 0400000
        Assert.Equal(0x80000, UnixNative.O_CLOEXEC);    // 02000000
        Assert.Equal(0x40, UnixNative.O_CREAT);         // 0100
        Assert.Equal(0x80, UnixNative.O_EXCL);          // 0200
    }

    [Fact]
    public void OpenDirectoryRefusesSymbolicLink()
    {
        using var root = new TemporaryRoot();
        Directory.CreateDirectory(root.Child("real"));
        Directory.CreateSymbolicLink(root.Child("link"), root.Child("real"));
        var failure = Assert.Throws<IOException>(() => UnixNative.OpenDirectory(root.Child("link")));
        Assert.NotEqual(0, failure.HResult);
    }

    [Fact]
    public void OpenRegularFileRefusesSymbolicLinkAndReportsAbsence()
    {
        using var root = new TemporaryRoot();
        var directory = UnixNative.OpenDirectory(root.Root);
        using (directory)
        {
            File.WriteAllText(root.Child("blob"), "payload");
            File.CreateSymbolicLink(root.Child("bloblink"), root.Child("blob"));
            Assert.Throws<IOException>(() => UnixNative.OpenRegularFile(directory, "bloblink", 4096));
            Assert.Null(UnixNative.OpenRegularFile(directory, "missing", 4096));
            using var stream = UnixNative.OpenRegularFile(directory, "blob", 4096);
            Assert.NotNull(stream);
        }
    }

    [Fact]
    public void CreatePrivateExclusiveIsSixHundredAndNoReplace()
    {
        using var root = new TemporaryRoot();
        var path = root.Child("reservation.json");
        using (var stream = UnixNative.CreatePrivateExclusive(path)) stream.WriteByte(1);
        // 0600 = 0x180: C# has no octal literal, and 0b110000 is not the same number.
        Assert.Equal(0x180, UnixNative.ModeBits(UnixNative.Stat(path).Mode));
        Assert.Throws<IOException>(() => UnixNative.CreatePrivateExclusive(path));
    }

    private sealed class TemporaryRoot : IDisposable
    {
        private readonly string _root = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"azki-test-{Guid.NewGuid():N}");
        public TemporaryRoot() => Directory.CreateDirectory(_root);
        public string Root => _root;
        public string Child(string name) => System.IO.Path.Combine(_root, name);
        public void Dispose() => Directory.Delete(_root, recursive: true);
    }
}

public class FrameReaderTests
{
    private static SessionManifest Manifest() =>
        new("0f6a1d3c-1111-2222-3333-444455556666", "control.sock", "frame.raw", 4, 4, "qemu", "", "");

    private static byte[] Header(uint sequence, string magic = "Q66FRAME")
    {
        var bytes = new byte[RawFrame.HeaderBytes + 4 * 4 * 2];
        System.Text.Encoding.ASCII.GetBytes(magic).CopyTo(bytes, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), sequence);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 4);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20), 4);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(24), 8);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(28), 1);
        return bytes;
    }

    [Fact]
    public void MidWriteIsSkippedWithoutThrowing()
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-frame-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            // An odd sequence is the seqlock's "producer is writing" half: macOS retries at
            // the next deadline, so the reader must return null rather than raise.
            File.WriteAllBytes(Path.Combine(root, "frame.raw"), Header(7));
            using var reader = new FrameReader(root, Manifest());
            Assert.Null(reader.ReadNext());
            File.WriteAllBytes(Path.Combine(root, "frame.raw"), Header(8));
            var frame = reader.ReadNext();
            Assert.NotNull(frame);
            Assert.Equal(8u, frame!.Sequence);
            Assert.Null(reader.ReadNext()); // unchanged sequence
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

public class JsonReadTests
{
    [Fact]
    public void WrongTypeIsAbsenceNotThrow()
    {
        var node = JsonNode.Parse("""{"text":"a","count":7,"flag":true,"list":["x",3]}""")!.AsObject();
        Assert.Equal("a", JsonRead.AsString(node["text"]));
        Assert.Null(JsonRead.AsString(node["count"]));
        Assert.Equal(7, JsonRead.AsInt(node["count"], -1));
        Assert.Equal(-1, JsonRead.AsInt(node["text"], -1));
        Assert.True(JsonRead.AsBool(node["flag"]));
        Assert.False(JsonRead.AsBool(node["count"]));
        Assert.Equal(new[] { "x" }, JsonRead.AsStringList(node["list"]));
        Assert.Empty(JsonRead.AsStringList(node["text"]));
    }
}

public class ConnectionSettingsTests
{
    private static string ScratchPath(out string root)
    {
        root = Path.Combine(Path.GetTempPath(), $"azki-settings-{Guid.NewGuid():N}");
        return Path.Combine(root, "settings.json");
    }

    [Fact]
    public void ValueSurvivesReloadAndRangeIsEnforced()
    {
        var path = ScratchPath(out var root);
        try
        {
            var first = new ConnectionSettings(path);
            Assert.Equal(ConnectionSettings.DefaultTimeoutSeconds, first.TimeoutSeconds);
            first.TimeoutSeconds = 45;
            // The Swift didSet drops an out-of-range write; the persisted value must not follow it.
            first.TimeoutSeconds = 0;
            first.TimeoutSeconds = 301;
            Assert.Equal(45, new ConnectionSettings(path).TimeoutSeconds);
            Assert.Equal(45d, first.TimeoutSecondsDisplay);
            first.TimeoutSecondsDisplay = 60.4;
            Assert.Equal(60, new ConnectionSettings(path).TimeoutSeconds);
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public void FileUsesTheMacOsUserDefaultsKeyAndStaysPrivate()
    {
        var path = ScratchPath(out var root);
        try
        {
            new ConnectionSettings(path) { TimeoutSeconds = 90 };
            Assert.Contains(ConnectionSettings.TimeoutKey, File.ReadAllText(path));
            Assert.Equal(0x180, UnixNative.ModeBits(UnixNative.Stat(path).Mode)); // 0600
            Assert.Equal(0x1C0, UnixNative.ModeBits(UnixNative.Stat(root).Mode)); // 0700
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public void DamagedFileFallsBackToDefaultAndHealsOnWrite()
    {
        var path = ScratchPath(out var root);
        try
        {
            Directory.CreateDirectory(root);
            foreach (var damaged in new[] { "not json", "[1,2]", """{"version":1}""" })
            {
                File.WriteAllText(path, damaged);
                Assert.Equal(ConnectionSettings.DefaultTimeoutSeconds, new ConnectionSettings(path).TimeoutSeconds);
            }
            var healed = new ConnectionSettings(path);
            healed.TimeoutSeconds = 120;
            Assert.Equal(120, new ConnectionSettings(path).TimeoutSeconds);
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}

public class RecordingTests
{
    private static bool FfmpegAvailable() =>
        (Environment.GetEnvironmentVariable("PATH") ?? string.Empty)
            .Split(Path.PathSeparator)
            .Any(directory => File.Exists(Path.Combine(directory, "ffmpeg")));

    private static RawFrame Frame(int width, int height)
    {
        var pixels = new byte[width * 2 * height];
        for (var index = 0; index < pixels.Length; index++) pixels[index] = (byte)(index % 251);
        return new RawFrame(2, width, height, width * 2, pixels);
    }

    [Fact]
    public void SafeNameMatchesTheMacOsContract()
    {
        // Same rules as SimulatorRecordingEncoder.safeName: keep alphanumerics and
        // " -_", replace everything else with "-", cap at 96 bytes, fall back to 设备.
        Assert.Equal("Q66 pro_1-A", FfmpegEncoder.SafeName("Q66 pro_1/A"));
        // A lone slash still yields a non-empty "-", exactly like the Swift trim;
        // only an all-whitespace name falls back to 设备.
        Assert.Equal("-", FfmpegEncoder.SafeName("  /  "));
        Assert.Equal("设备", FfmpegEncoder.SafeName("   "));
        Assert.Equal(96, Encoding.UTF8.GetByteCount(FfmpegEncoder.SafeName(new string('a', 400))));
        // Non-ASCII names survive as letters (char.IsLetterOrDigit), not dashes.
        Assert.Equal("小扣", FfmpegEncoder.SafeName("小扣"));
    }

    [Fact]
    public void OddGeometryIsRefusedBeforeAnythingIsCreated()
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-record-{Guid.NewGuid():N}");
        try
        {
            var failure = Assert.Throws<IOException>(() => FfmpegEncoder.Launch("ffmpeg", 33, 32, 30, root, "x"));
            Assert.Contains("不适合", failure.Message);
            // The guard runs before anything is created, so the output root stays absent.
            Assert.False(Directory.Exists(root));
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public void MissingFfmpegRefusesWithoutStagingRemnants()
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-record-{Guid.NewGuid():N}");
        try
        {
            var failure = Assert.Throws<IOException>(() => FfmpegEncoder.Launch(
                Path.Combine(root, "definitely-not-here"), 64, 64, 30, root, "x"));
            Assert.Contains("未找到 ffmpeg", failure.Message);
            Assert.Empty(Directory.GetDirectories(root, ".azki-recording-*"));
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public void StartRefusesWithoutAFrame()
    {
        var model = new RecordingModel(Path.GetTempPath());
        Assert.False(model.Start("x", () => null));
        Assert.Contains("请等待设备启动", model.ErrorMessage);
        Assert.False(model.IsRecording);
    }

    [Fact]
    public async Task RecordsThroughFfmpegWhenAvailable()
    {
        // Returning quietly here would be counted as a pass, so a missing encoder says so:
        // ffmpeg is in the documented build dependencies of both the Arch container and a
        // developer machine, and this is the only test that watches the real pipe.
        Assert.True(FfmpegAvailable(), "需要 ffmpeg 才能验真录屏链路（安装 ffmpeg 后重跑）");
        var root = Path.Combine(Path.GetTempPath(), $"azki-record-{Guid.NewGuid():N}");
        try
        {
            var frame = Frame(64, 64);
            var model = new RecordingModel(root);
            Assert.True(model.Start("测试 实例", () => frame));
            await Task.Delay(250);
            var first = model.StopAsync();
            var second = model.StopAsync(); // concurrent stops share one finalization
            await Task.WhenAll(first, second);
            Assert.False(model.IsRecording);
            Assert.False(model.IsFinishing);
            Assert.NotNull(model.SavedFile);
            Assert.True(File.Exists(model.SavedFile));
            Assert.StartsWith("Azki-", Path.GetFileName(model.SavedFile!));
            Assert.Contains("测试 实例", Path.GetFileName(model.SavedFile!));
            Assert.Empty(Directory.GetDirectories(root, ".azki-recording-*"));
            Assert.Equal(0x180, UnixNative.ModeBits(UnixNative.Stat(model.SavedFile!).Mode)); // 0600
            var startInfo = new ProcessStartInfo("ffprobe") { RedirectStandardOutput = true, UseShellExecute = false };
            // ArgumentList, not an argument string: the .NET parser only understands
            // double quotes, and this path must arrive byte-for-byte.
            foreach (var argument in new[]
            {
                "-v", "error", "-select_streams", "v:0",
                "-show_entries", "stream=codec_name,width,height", "-of", "csv=p=0", model.SavedFile!,
            }) startInfo.ArgumentList.Add(argument);
            var probe = Process.Start(startInfo)!;
            var output = await probe.StandardOutput.ReadToEndAsync();
            await probe.WaitForExitAsync();
            Assert.Equal(0, probe.ExitCode);
            Assert.Contains("h264", output);
            Assert.Contains("64,64", output);
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    /// <summary>The pipe is framed by hand: rgb565le is width*2 bytes per row with no
    /// padding, so a wrong stride shows up as a frame count that does not match what was
    /// written rather than as an error. Feeding a known count makes that the assertion.</summary>
    [Fact]
    public async Task EveryFedFrameSurvivesTheRawVideoPipe()
    {
        Assert.True(FfmpegAvailable(), "需要 ffmpeg 才能验真录屏链路（安装 ffmpeg 后重跑）");
        var root = Path.Combine(Path.GetTempPath(), $"azki-record-{Guid.NewGuid():N}");
        try
        {
            const int frames = 12;
            var encoder = FfmpegEncoder.Launch("ffmpeg", 64, 64, 30, root, "count");
            for (var index = 0; index < frames; index++)
            {
                var pixels = new byte[64 * 64 * 2];
                for (var byteIndex = 0; byteIndex < pixels.Length; byteIndex++)
                    pixels[byteIndex] = (byte)((byteIndex + index * 7) % 251);
                await encoder.WriteAsync(pixels, CancellationToken.None);
            }
            var path = await encoder.FinishAsync();
            var probed = await ProbeAsync(path, "stream=codec_name,width,height,nb_frames");
            Assert.Contains("h264", probed);
            Assert.True(probed.Contains($",{frames},"), $"frame count lost through the pipe: {probed}");
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static async Task<string> ProbeAsync(string path, string entries)
    {
        var startInfo = new ProcessStartInfo("ffprobe") { RedirectStandardOutput = true, UseShellExecute = false };
        foreach (var argument in new[]
        { "-v", "error", "-select_streams", "v:0", "-show_entries", entries, "-of", "csv=p=0", path })
        {
            startInfo.ArgumentList.Add(argument);
        }
        var probe = Process.Start(startInfo)!;
        var output = await probe.StandardOutput.ReadToEndAsync();
        await probe.WaitForExitAsync();
        Assert.Equal(0, probe.ExitCode);
        return output.Replace("\n", ",");
    }
}
