using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// Opt-in integration test over the real bwrap + bundled-python CLI that the
/// data panel and the firmware-settings sheet use for next-boot persistence.
/// It runs only when AZKI_APP_ROOT points at an extracted release package
/// (scripts/build-linux.sh output) and refuses to pretend otherwise; CI excludes
/// it by category because a container job cannot unshare a user namespace. This is
/// the strongest end-to-end available without firmware: it proves the sandboxed
/// round trip, the write grants the command needs, and the receipt shape the panels
/// parse.
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class InstanceSettingsCliTests
{
    private static EngineLayout? ResolveLayout() => EngineLayout.TryResolve();

    [Fact]
    [Trait("Category", "Integration")]
    public async Task RoundTripsSettingsThroughTheSandboxedCli()
    {
        // A quiet return here would be counted as a pass, so a run without the package
        // says so out loud; CI excludes this by category instead.
        var layout = ResolveLayout();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件（scripts/build-linux.sh 的产物）");
        var root = Path.Combine(Path.GetTempPath(), $"azki-cli-{Guid.NewGuid():N}");
        var previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", root);
        try
        {
            var runner = new InspectionRunner(layout);
            var id = Guid.NewGuid().ToString();
            var read = SettingsReceipt.Decode(await runner.RunCheckedAsync(
                "instance-settings", ["--instance-id", id]));
            Assert.Equal(1, read.Settings.Schema);
            Assert.Null(read.Settings.Battery);

            read.Settings.Battery = new InstanceFirmwareSettings.BatterySettings { Level = 42, Charger = "charging" };
            read.Settings.Health = new InstanceFirmwareSettings.HealthSettings { HeartRate = 96, Worn = false };
            read.Settings.Properties["persist.integration"] = "ok";
            var saved = SettingsReceipt.Decode(await runner.RunCheckedAsync("instance-settings",
                ["--instance-id", id, "--set", read.Settings.Encode().ToJsonString()]));
            Assert.Equal(42, saved.Settings.Battery!.Level);
            Assert.Equal("charging", saved.Settings.Battery.Charger);
            Assert.Equal(96, saved.Settings.Health!.HeartRate);
            Assert.False(saved.Settings.Health.Worn!.Value);
            Assert.Equal("ok", saved.Settings.Properties["persist.integration"]);
            // The written document must also be valid for the live engine reader:
            // a second describe returns the same values, proving validate() accepted it.
            var again = SettingsReceipt.Decode(await runner.RunCheckedAsync(
                "instance-settings", ["--instance-id", id]));
            Assert.Equal(42, again.Settings.Battery!.Level);
        }
        finally
        {
            Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", previous);
            try { Directory.Delete(root, recursive: true); } catch (IOException) { }
        }
    }
}
