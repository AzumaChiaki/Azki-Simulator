using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The plan's end-to-end ladder for one real firmware image, written as a single
/// test so the acceptance cannot quietly shrink later; the steps themselves live
/// in <see cref="BootLadder"/> and are shared with the synthetic-guest run.
/// The repository carries no firmware, so this is opt-in through AZKI_FIRMWARE
/// and refuses to pretend when it is absent.
/// </summary>
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class FirmwareBootTests
{
    [Fact]
    [Trait("Category", "Integration")]
    public async Task BootsAuthorizedFirmwareAndLeavesNothingBehind()
    {
        var firmware = Environment.GetEnvironmentVariable("AZKI_FIRMWARE");
        Assert.True(!string.IsNullOrEmpty(firmware) && File.Exists(firmware),
            "需要 AZKI_FIRMWARE 指向一份已授权的 OTA 固件包（仓库内不含固件）");
        var layout = EngineLayout.TryResolve();
        Assert.True(layout is not null, "需要 AZKI_APP_ROOT 指向解包后的发布件");
        await BootLadder.RunAsync(layout!, firmware!);
    }
}
