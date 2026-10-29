using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The same boot ladder, against a guest this repository can build itself.
///
/// What this proves is the Linux port: a prepared session, a bwrap-wrapped
/// runner that receives the AP-derived board geometry, a core that repaints,
/// frames that reach the shared file, an accepted touch, a growing log, a PNG
/// screenshot and a stop that leaves no emulator behind. What it does not
/// prove is anything about the real firmware — the guest is a 230-byte test
/// double, so a green run here is never a substitute for the authorized
/// ladder in <see cref="FirmwareBootTests"/>.
/// </summary>
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class SyntheticGuestBootTests
{
    [Fact]
    [Trait("Category", "Integration")]
    public async Task BootsSyntheticGuestAndLeavesNothingBehind()
    {
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        var directory = Path.Combine(Path.GetTempPath(), $"azki-synth-{Guid.NewGuid():N}");
        try
        {
            var ota = SyntheticGuest.WriteOta(directory);
            await BootLadder.RunAsync(layout!, ota);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch (IOException) { }
        }
    }
}
