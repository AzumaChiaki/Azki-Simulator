using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The import is the first step of the boot ladder and the only one that does
/// not need the real firmware: the probe is structural, so a synthetic package
/// carries the same shape as a real OTA. This runs that step, and the
/// preparation after it, through the real bwrap sandbox and the packaged CLI,
/// which is the part of the ladder that can be proven without an authorized
/// image.
/// </summary>
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class FirmwareImportCliTests
{
    [Fact]
    [Trait("Category", "Integration")]
    public async Task ImportsAndListsASyntheticPackageThroughTheSandbox()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        using var scratch = new ScratchDataRoot();
        var source = Path.Combine(scratch.Root, "ota.zip");
        WriteSyntheticOta(source, SyntheticAp());

        var runner = new InspectionRunner(layout!);
        var imported = await runner.RunCheckedAsync("library-import", new[] { "--input", source });
        Assert.True(JsonRead.AsBool(imported["recognized"]), "完整 OTA 包没有被识别");
        Assert.True(JsonRead.AsBool(imported["imported"]));

        // The receipt contract the macOS decoder enforces: the candidates are
        // not empty, every candidate has an item, and the selection is only
        // left open when there is more than one candidate.
        var candidates = Candidates(imported);
        Assert.NotEmpty(candidates);
        var itemIds = (imported["items"] as JsonArray ?? new JsonArray())
            .OfType<JsonObject>()
            .Select(item => JsonRead.AsString(item["id"]))
            .Where(id => id is not null)
            .Select(id => id!)
            .ToHashSet();
        Assert.True(candidates.All(itemIds.Contains), "导入回执里的候选没有对应的条目");
        Assert.True(JsonRead.AsBool(imported["needsSelection"]) || candidates.Count == 1,
            "候选多于一个时必须把选择留给用户");

        var catalog = await runner.RunCheckedAsync("library-catalog", Array.Empty<string>());
        var listed = Ready(catalog);
        foreach (var candidate in candidates)
        {
            Assert.Contains(candidate, listed);
        }

        // The import is reclaimed, so the private library does not keep the
        // objects of a package nobody uses.
        var digest = JsonRead.AsString(imported["sha256"]);
        Assert.True(digest is not null && digest.Length == 64);
        await runner.RunCheckedAsync("library-delete", new[] { "--sha256", digest! });
        var after = await runner.RunCheckedAsync("library-catalog", Array.Empty<string>());
        Assert.Empty((after["items"] as JsonArray ?? new JsonArray()).OfType<JsonObject>());
    }

    [Fact]
    [Trait("Category", "Integration")]
    public async Task PreparesTheImportedProfileThroughTheSandbox()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        using var scratch = new ScratchDataRoot();
        var source = Path.Combine(scratch.Root, "ota.zip");
        WriteSyntheticOta(source, SyntheticAp());

        var runner = new InspectionRunner(layout!);
        var imported = await runner.RunCheckedAsync("library-import", new[] { "--input", source });
        var profile = Candidates(imported).First();

        // The preparation materializes the engine, writes the recipe for the
        // selected preset and opens the session the emulator would run in.
        // Without an instance identity the storage is not instance-scoped, and
        // that part needs the real firmware's partition layout, so it stays out
        // of this chain.
        var sessionRoot = DataLocations.NewSessionRoot();
        try
        {
            var prepared = await PrepareAsync(runner, profile, sessionRoot);
            Assert.True(JsonRead.AsBool(prepared["ok"]));
            Assert.True(JsonRead.AsBool(prepared["runtimeReady"]), "准备之后运行时没有就绪");
            Assert.Equal(profile, JsonRead.AsString(prepared["profileID"]));

            var session = Path.Combine(sessionRoot, "session");
            Assert.True(File.Exists(Path.Combine(session, "session.json")), "准备之后没有会话描述");
            Assert.True(File.Exists(Path.Combine(session, "backend-plan.json")), "准备之后没有后端计划");
            Assert.True(Directory.Exists(Path.Combine(sessionRoot, "prepared")), "准备之后没有落地输入");
        }
        finally
        {
            try { Directory.Delete(sessionRoot, recursive: true); } catch (IOException) { }
        }
    }

    /// <summary>The host reserves the session root before the command runs; the
    /// command refuses to work in a directory it did not create itself.</summary>
    private static async Task<JsonNode> PrepareAsync(InspectionRunner runner, string profile, string sessionRoot)
    {
        DataLocations.EnsurePrivateDirectory(sessionRoot);
        var stat = UnixNative.Stat(sessionRoot);
        var nonce = Guid.NewGuid().ToString("N");
        var reservationPath = Path.Combine(sessionRoot, ".reservation.json");
        using (var handle = UnixNative.CreatePrivateExclusive(reservationPath))
            JsonSerializer.Serialize(handle, new { version = 1, nonce, device = stat.Device, inode = stat.Inode });
        return await runner.RunCheckedAsync("prepare-bundled", new[]
        {
            "--profile-id", profile, "--session-root", sessionRoot, "--reservation-nonce", nonce,
        });
    }

    private static List<string> Candidates(JsonNode receipt) =>
        (receipt["candidateIDs"] as JsonArray ?? new JsonArray())
            .OfType<JsonNode>()
            .Select(node => JsonRead.AsString(node))
            .Where(id => id is not null)
            .Select(id => id!)
            .ToList();

    private static List<string> Ready(JsonNode catalog) =>
        (catalog["items"] as JsonArray ?? new JsonArray())
            .OfType<JsonObject>()
            .Where(item => JsonRead.AsBool(item["availability"]?["ready"]))
            .Select(item => JsonRead.AsString(item["id"]))
            .Where(id => id is not null)
            .Select(id => id!)
            .ToList();

    /// <summary>A boot header in the shape the probe reads: the chip line, the
    /// partition offsets and the reset sequence with a stack inside the modelled
    /// RAM. It is a test double, not firmware: nothing here can boot.</summary>
    private static byte[] SyntheticAp()
    {
        var data = new byte[4096];
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(12), 0x28000000u + 100);
        BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(16), 0x4802);
        data[18] = 0x80; data[19] = 0xF3; data[20] = 0x08; data[21] = 0x88;
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(0x1c), 0x20100000u);
        var header = Encoding.ASCII.GetBytes(
            "\nCHIP=best1503\nOTA_CODE_OFFSET=0x1000\nFLASH_SIZE=0x1000000\nVERSION=4.100.177\n");
        Array.Copy(header, 0, data, 100, header.Length);
        return data;
    }

    private static void WriteSyntheticOta(string path, byte[] ap)
    {
        using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
        Write(archive, "vela_ap.bin", ap);
        Write(archive, "vela_resource.bin", Encoding.UTF8.GetBytes(new string('R', 512)));
        Write(archive, "version.ini", Encoding.UTF8.GetBytes("sw_version=4.100.177\n"));
    }

    private static void Write(ZipArchive archive, string name, byte[] payload)
    {
        var entry = archive.CreateEntry(name);
        using var stream = entry.Open();
        stream.Write(payload, 0, payload.Length);
    }

    private sealed class ScratchDataRoot : IDisposable
    {
        private readonly string? _previous;

        public ScratchDataRoot()
        {
            _previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
            Root = Path.Combine(Path.GetTempPath(), $"azki-import-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Root);
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", Root);
        }

        public string Root { get; }

        public void Dispose()
        {
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", _previous);
            try { Directory.Delete(Root, recursive: true); } catch (IOException) { }
        }
    }
}
