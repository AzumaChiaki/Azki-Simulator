using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The host half of session cleanup. The sandbox has already decided a root is safe to
/// lose and could only finish the job if it owned the parent directory, so this helper
/// takes the empty shell on its behalf. Every refusal below is the interesting part: a
/// helper that deletes a little too eagerly would be reaching past what the sandbox saw.
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class SessionCleanupTests : IDisposable
{
    private readonly string _root;
    private readonly string? _previous;

    public SessionCleanupTests()
    {
        _root = Path.Combine(Path.GetTempPath(), $"azki-tmp-{Guid.NewGuid():N}");
        Directory.CreateDirectory(_root);
        _previous = Environment.GetEnvironmentVariable("AZKI_TMP_ROOT");
        Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", _root);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", _previous);
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    private string Make(string name, bool withContent = false)
    {
        var path = Path.Combine(_root, name);
        Directory.CreateDirectory(path);
        if (withContent) File.WriteAllText(Path.Combine(path, "holder.bin"), "x");
        return path;
    }

    private static JsonNode Receipt(params (string Path, string Reason)[] kept) => new JsonObject
    {
        ["ok"] = true,
        ["kept"] = new JsonArray(kept.Select(entry => JsonValue.Create(entry.Path)).ToArray()),
        ["reasons"] = kept.Aggregate(new JsonObject(), (map, entry) => { map[entry.Path] = entry.Reason; return map; }),
    };

    private const string Busy = "remove-failed: Device or resource busy";

    [Fact]
    public void TakesTheEmptyShellTheSandboxCouldNotUnlink()
    {
        var path = Make("azki-b-empty");
        Assert.Equal([path], SessionCleanup.Reclaim(Receipt((path, Busy)), [path]));
        Assert.False(Directory.Exists(path));
    }

    [Fact]
    public void LeavesRootsTheSandboxChoseToKeep()
    {
        // A root that is merely empty and recent may be a session being prepared right
        // now; only a path the sandbox tried and failed to unlink is ours to finish.
        var preparing = Make("azki-b-preparing");
        Assert.Empty(SessionCleanup.Reclaim(Receipt((preparing, "recent")), [preparing]));
        Assert.True(Directory.Exists(preparing));

        var running = Make("azki-b-running");
        Assert.Empty(SessionCleanup.Reclaim(Receipt((running, "running")), [running]));
        Assert.True(Directory.Exists(running));
    }

    [Fact]
    public void OnlyActsOnRootsItHandedToTheSandbox()
    {
        var path = Make("azki-b-ungranted");
        Assert.Empty(SessionCleanup.Reclaim(Receipt((path, Busy)), Array.Empty<string>()));
        Assert.True(Directory.Exists(path));
    }

    [Fact]
    public void RefusesAnythingThatCouldStillHoldData()
    {
        var filled = Make("azki-b-filled", withContent: true);
        var otherName = Make("not-azki-keepme");
        var nested = Path.Combine(Make("outer"), "azki-b-nested");
        Directory.CreateDirectory(nested);
        var file = Path.Combine(_root, "azki-b-afile");
        File.WriteAllText(file, "x");
        var target = Make("azki-b-target");
        var link = Path.Combine(_root, "azki-b-link");
        File.CreateSymbolicLink(link, target);

        // The target is deliberately absent from the receipt: the sandbox was shown the
        // link path, so finishing that job must never reach what the link points at.
        var receipt = Receipt((filled, Busy), (otherName, Busy), (nested, Busy), (file, Busy), (link, Busy));
        Assert.Empty(SessionCleanup.Reclaim(receipt, [filled, otherName, nested, file, link]));
        Assert.True(Directory.Exists(filled));
        Assert.True(Directory.Exists(otherName));
        Assert.True(Directory.Exists(nested));
        Assert.True(File.Exists(file));
        Assert.NotNull(new FileInfo(link).LinkTarget);
        Assert.True(Directory.Exists(target));
    }

    [Fact]
    public void ToleratesReceiptsWithNothingToRemoveAndPathsThatVanished()
    {
        var gone = Path.Combine(_root, "azki-b-gone");
        Assert.Empty(SessionCleanup.Reclaim(Receipt(), [gone]));
        Assert.Empty(SessionCleanup.Reclaim(null, [gone]));
        Assert.Empty(SessionCleanup.Reclaim(Receipt((gone, Busy)), [gone]));
        var outside = Path.Combine(Path.GetTempPath(), $"azki-b-outside-{Guid.NewGuid():N}");
        Assert.Empty(SessionCleanup.Reclaim(Receipt((outside, Busy)), [outside]));
        Assert.False(Directory.Exists(outside));
    }

    [Fact]
    public void OnlyConsidersTheNamesTheGrantTableAdvertises()
    {
        Assert.Equal(["azki-b-", "azki-sim-", "azki-cmd-"], CliGrants.SessionPrefixes);
        foreach (var prefix in CliGrants.SessionPrefixes)
        {
            var path = Make(prefix + "x");
            Assert.Equal([path], SessionCleanup.Reclaim(Receipt((path, Busy)), [path]));
        }
        var lookalike = Make("azki-bx-lookalike");
        Assert.Empty(SessionCleanup.Reclaim(Receipt((lookalike, Busy)), [lookalike]));
        Assert.True(Directory.Exists(lookalike));
    }
}
