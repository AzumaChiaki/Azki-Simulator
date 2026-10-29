using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The plan's end-to-end ladder, in one place so the authorized-firmware check
/// and the synthetic-guest check cannot drift apart: import, take the profile
/// the catalog actually offers, prepare, start, watch frames advance, push a
/// touch, stream logs, capture a PNG and stop with no emulator left behind.
/// </summary>
internal static class BootLadder
{
    private static readonly TimeSpan BootDeadline = TimeSpan.FromSeconds(120);
    private static readonly TimeSpan FrameDeadline = TimeSpan.FromSeconds(60);

    public static async Task RunAsync(EngineLayout layout, string firmware)
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-boot-{Guid.NewGuid():N}");
        var previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", root);
        // Only processes this run created may show up as leaked; a second session already
        // running on the machine is not this ladder's responsibility.
        var pidsBefore = EmulatorPids();
        try
        {
            var runner = new InspectionRunner(layout);
            var imported = await runner.RunCheckedAsync("library-import", new[] { "--input", firmware });
            // Without --model the CLI leaves an ambiguous import as a question instead of a
            // profile. The ladder must say so rather than fail later with "no ready profile".
            var candidates = (imported["candidateIDs"] as JsonArray ?? new JsonArray())
                .Select(item => JsonRead.AsString(item) ?? "?");
            Assert.True(!JsonRead.AsBool(imported["needsSelection"]),
                $"该 OTA 匹配多个机型，需先用 --model 绑定：{string.Join(", ", candidates)}");
            var catalog = await runner.RunCheckedAsync("library-catalog", Array.Empty<string>());
            var profileId = ReadyProfiles(catalog).FirstOrDefault();
            Assert.True(profileId is not null, "导入后目录里没有可启动的 profile");

            await using var supervisor = await SessionSupervisor.StartAsync(layout,
                Guid.NewGuid().ToString(), profileId!, null, null, skipOobe: true, CancellationToken.None);
            using var client = await supervisor.LaunchAsync();

            await WaitUntilRunning(client);
            using var reader = new FrameReader(supervisor.SessionDirectory, client.Manifest);
            var first = await ReadFrameAsync(reader, BootDeadline);
            Assert.True(first.Pixels.Length == first.Stride * first.Height && first.Pixels.Length > 0);

            // The reader skips the sequence it already handed out, so a non-null result here
            // is a genuinely newer repaint. A guest parked on a static screen would fail this
            // the same way the macOS canvas would show a stale image, which is the point.
            var second = await ReadFrameAsync(reader, FrameDeadline);
            Assert.NotEqual(first.Sequence, second.Sequence);

            // A tap at the centre of the panel. The bridge refuses coordinates outside the
            // session display and requires a running core, so an accepted press+lift is the
            // assertion; whether the guest reacted is a human judgement on the recorded frames.
            await Task.Run(() =>
            {
                client.Input(first.Width / 2, first.Height / 2, true);
                client.Input(first.Width / 2, first.Height / 2, false);
            });

            var logs = await Task.Run(() => client.Command("logs", LogStream.Request(null)));
            var (cursor, fault) = LogStream.Advance(logs);
            Assert.True(fault is null, $"增量日志游标被拒绝：{fault}");
            if (cursor is not null)
                await Task.Run(() => client.Command("logs", LogStream.Request(cursor)));

            var shot = await Task.Run(() => client.Command("screenshot"));
            var name = JsonRead.AsString(shot["file"]);
            Assert.True(!string.IsNullOrEmpty(name) && !name!.Contains('/'), "截图没有返回会话内文件名");
            var bytes = await File.ReadAllBytesAsync(Path.Combine(supervisor.SessionDirectory, name!));
            Assert.True(bytes.Length > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G',
                "截图不是 PNG");
            Assert.Equal(JsonRead.AsString(shot["sha256"]),
                Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
            Assert.Equal(first.Width, JsonRead.AsInt(shot["width"]));
            Assert.Equal(first.Height, JsonRead.AsInt(shot["height"]));

            await Task.Run(() => client.Command("stop"));
        }
        finally
        {
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", previous);
            try { Directory.Delete(root, recursive: true); } catch (IOException) { }
        }
        var leaked = EmulatorPids().Except(pidsBefore);
        Assert.True(!leaked.Any(), $"停止后仍有 qemu-system-arm 残留：{string.Join(' ', leaked)}");
    }

    private static IEnumerable<string> ReadyProfiles(JsonNode catalog) =>
        (catalog["items"] as JsonArray ?? new JsonArray()).OfType<JsonObject>()
            .Where(item => JsonRead.AsBool(item["availability"]?["ready"]))
            .Select(item => JsonRead.AsString(item["id"]))
            .Where(id => id is not null)!
            .Select(id => id!);

    private static async Task WaitUntilRunning(BridgeClient client)
    {
        var deadline = DateTime.UtcNow + BootDeadline;
        while (DateTime.UtcNow < deadline)
        {
            var status = await Task.Run(() => client.Command("status"));
            if (JsonRead.AsString(status["state"]) == "running") return;
            await Task.Delay(500);
        }
        throw new TimeoutException($"guest 未在 {BootDeadline.TotalSeconds:F0} 秒内进入 running");
    }

    private static async Task<RawFrame> ReadFrameAsync(FrameReader reader, TimeSpan budget)
    {
        var deadline = DateTime.UtcNow + budget;
        while (DateTime.UtcNow < deadline)
        {
            var frame = await Task.Run(reader.ReadNext);
            if (frame is not null) return frame;
            await Task.Delay(50);
        }
        throw new TimeoutException($"{budget.TotalSeconds:F0} 秒内没有读到有效画面");
    }

    private static HashSet<int> EmulatorPids() =>
        Process.GetProcessesByName("qemu-system-arm").Select(process => process.Id).ToHashSet();
}
