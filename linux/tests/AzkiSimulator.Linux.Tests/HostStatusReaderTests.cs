using AzkiSimulator.Linux.Core;
using System.Text.Json.Nodes;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>The reader must accept exactly what the macOS publisher writes and reject
/// everything else without throwing — a foreign or damaged sharing directory sits in
/// a shared /tmp and is the normal case, not the exception.</summary>
public class HostStatusReaderTests
{
    private sealed record Fixture(string Root, string SessionPath, string Id) : IDisposable
    {
        public void Dispose() => Directory.Delete(Root, recursive: true);
    }

    private static string SessionJson(string id, string directory, string protocol) =>
        new JsonObject
        {
            ["version"] = 1,
            ["id"] = id,
            ["directory"] = directory,
            ["application"] = "AzkiSimulator",
            ["protocol"] = protocol,
            ["transport"] = "file-queue",
            ["capabilities"] = new JsonArray("status", "logs"),
            ["requests"] = "requests",
            ["responses"] = "responses",
            ["lease"] = "lease.json",
        }.ToJsonString();

    private static string LeaseJson(string id, int sequence, int expiresAt) =>
        new JsonObject { ["version"] = 1, ["id"] = id, ["sequence"] = sequence, ["expiresAt"] = expiresAt }.ToJsonString();

    /// <summary>Mirrors SimulatorStatusFilePublisher.Session.create on disk: 0700 dirs,
    /// 0600 single-link files, lowercase-UUID directory name.</summary>
    private static Fixture Create(string id = "", int leaseDelta = 3, string protocol = "azki.simulator.status-files.v1")
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-status-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        File.SetUnixFileMode(root, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        // The publisher puts the lowercase uuidString in every field, and the
        // directory name repeats it; mixed case anywhere is not one of its sessions.
        var uuid = (id.Length == 0 ? Guid.NewGuid().ToString("D") : id).ToLowerInvariant();
        var sessionPath = Path.Combine(root, "azki-host-" + uuid);
        Directory.CreateDirectory(sessionPath);
        File.SetUnixFileMode(sessionPath, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        Directory.CreateDirectory(Path.Combine(sessionPath, "requests"));
        Directory.CreateDirectory(Path.Combine(sessionPath, "responses"));
        var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        WritePrivate(Path.Combine(sessionPath, "session.json"), SessionJson(uuid, sessionPath, protocol));
        WritePrivate(Path.Combine(sessionPath, "lease.json"), LeaseJson(uuid, 7, (int)now + leaseDelta));
        return new Fixture(root, sessionPath, uuid);
    }

    private static void WritePrivate(string path, string text)
    {
        File.WriteAllText(path, text);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
    }

    [Fact]
    public void ValidSessionIsListedWithActiveLease()
    {
        using var fixture = Create();
        var session = Assert.Single(HostStatusReader.Scan(fixture.Root));
        Assert.True(session.LeaseActive);
        Assert.Equal(7, session.LeaseSequence);
        Assert.StartsWith("status v1", session.Summary);
        Assert.Contains("status/logs", session.Summary);
    }

    [Fact]
    public void StudioProtocolIsNamedAndPastExpiryStaysVisibleAsExpired()
    {
        using var studio = Create(protocol: "azki.simulator.studio-files.v1");
        using var expired = Create(leaseDelta: -1);
        Assert.Contains("studio v1", Assert.Single(HostStatusReader.Scan(studio.Root)).Summary);
        var dying = Assert.Single(HostStatusReader.Scan(expired.Root));
        Assert.False(dying.LeaseActive);
        Assert.Contains("已过期", dying.Summary);
    }

    [Fact]
    public void ForeignOrDamagedEntriesAreSkippedSilently()
    {
        using var notGuid = Create(id: "not-a-uuid");
        // A real publisher leases five seconds, never sixty: a far-future lease is
        // forged or broken, not live sharing.
        using var farFuture = Create();
        WritePrivate(Path.Combine(farFuture.SessionPath, "lease.json"),
            LeaseJson(farFuture.Id, 1, (int)DateTimeOffset.UtcNow.ToUnixTimeSeconds() + 60));
        using var badManifest = Create();
        WritePrivate(Path.Combine(badManifest.SessionPath, "session.json"), "{\"version\":2}");
        using var badLease = Create();
        WritePrivate(Path.Combine(badLease.SessionPath, "lease.json"), "not json");
        using var worldWritable = Create();
        // OtherRead alone would stay acceptable; it is the writable bit that makes a
        // shared file untrustworthy, matching the publisher's own check.
        File.SetUnixFileMode(Path.Combine(worldWritable.SessionPath, "session.json"),
            UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.OtherWrite);
        using var symlinked = Create();
        Directory.Move(symlinked.SessionPath, symlinked.SessionPath + "-real");
        Directory.CreateSymbolicLink(symlinked.SessionPath, symlinked.SessionPath + "-real");
        foreach (var fixture in new[] { notGuid, farFuture, badManifest, badLease, worldWritable, symlinked })
            Assert.Empty(HostStatusReader.Scan(fixture.Root));
    }

    [Fact]
    public void MissingTmpRootScansEmpty() =>
        Assert.Empty(HostStatusReader.Scan(Path.Combine(Path.GetTempPath(), $"azki-status-missing-{Guid.NewGuid():N}")));
}
