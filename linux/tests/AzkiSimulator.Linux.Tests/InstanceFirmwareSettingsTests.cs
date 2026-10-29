using System.Text.Json.Nodes;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>The settings document and the live matrix are the two places where a
/// silently wrong decode corrupts guest state without any crash, so every guard
/// the Swift receipt validator has is pinned here too.</summary>
public class InstanceFirmwareSettingsTests
{
    [Fact]
    public void RoundTripPreservesEveryField()
    {
        var document = JsonNode.Parse("""
        {"schema":1,
         "properties":{"persist.locale":"zh_CN","persist.clearme":null},
         "clock":{"mode":"offset","offsetSeconds":-3600},
         "battery":{"level":42,"sampleRaw":3900,"charger":"charging","gauge":"hp4560"},
         "health":{"walkSteps":500,"cadence":96,"heartRate":88,"worn":false}}
        """)!;
        var settings = InstanceFirmwareSettings.Decode(document);
        Assert.Equal("offset", settings.ClockMode);
        Assert.Equal(-3600, settings.ClockOffsetSeconds);
        Assert.Equal(42, settings.Battery!.Level);
        Assert.Equal("hp4560", settings.Battery.Gauge);
        Assert.Equal(500, settings.Health!.WalkSteps);
        Assert.False(settings.Health.Worn!.Value);
        var again = InstanceFirmwareSettings.Decode(settings.Encode());
        Assert.Equal("zh_CN", again.Properties["persist.locale"]);
        Assert.True(again.Properties.ContainsKey("persist.clearme"));
        Assert.Null(again.Properties["persist.clearme"]);
        Assert.Equal(3900, again.Battery!.SampleRaw);
        Assert.Equal(88, again.Health!.HeartRate);
    }

    [Fact]
    public void LegacyStepsAliasAndEmptyHealthAreHandled()
    {
        var settings = InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"health":{"steps":120}}""")!);
        // "steps" is the old key for walkSteps.
        Assert.Equal(120, settings.Health!.WalkSteps);
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"health":{"cadence":201}}""")!));
    }

    [Fact]
    public void DamagedDocumentsAreRefused()
    {
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(JsonNode.Parse("""{"schema":2}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"properties":{"bad.key":"x"}}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"properties":{"persist.a":12}}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"battery":{"level":101}}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"battery":{"charger":"mystery"}}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"health":{"heartRate":300}}""")!));
        Assert.Throws<InvalidDataException>(() => InstanceFirmwareSettings.Decode(
            JsonNode.Parse("""{"schema":1,"clock":{"mode":"sundial"}}""")!));
        Assert.True(InstanceFirmwareSettings.Decode(JsonNode.Parse("""{"schema":1,"health":{"cadence":null}}""")!).Health is null);
    }

    [Fact]
    public void EncodingMatchesTheHelpersExactKeySets()
    {
        // The Python validator requires clock == {"mode"} unless offset, and omits
        // absent battery fields; a JsonObject indexer assigned null KEEPS the key,
        // so this pins the exact sets without needing the sandboxed CLI.
        var encoded = new InstanceFirmwareSettings().Encode();
        Assert.Equal(new[] { "schema", "properties", "clock" }, encoded.Select(pair => pair.Key).ToArray());
        Assert.Equal(new[] { "mode" }, encoded["clock"]!.AsObject().Select(pair => pair.Key).ToArray());
        var withBattery = new InstanceFirmwareSettings
        {
            Battery = new InstanceFirmwareSettings.BatterySettings { Charger = "charging" },
            ClockMode = "offset",
            ClockOffsetSeconds = -60,
        }.Encode();
        Assert.Equal(new[] { "mode", "offsetSeconds" }, withBattery["clock"]!.AsObject().Select(pair => pair.Key).ToArray());
        Assert.Equal(new[] { "charger", "gauge" }, withBattery["battery"]!.AsObject().Select(pair => pair.Key).ToArray());
    }

    [Fact]
    public void ReceiptGatesRowsByModelPrefix()
    {
        var receipt = SettingsReceipt.Decode(JsonNode.Parse("""
        {"ok":true,"version":1,"settings":{"schema":1},
         "observed":{"properties":{"persist.locale":"en_US"},"recordedAtUnix":1},
         "batteryInputs":[{"modelPrefix":"miwear.watch.q66","level":true,"charger":false}],
         "healthInputs":[{"modelPrefix":"miwear.watch.q66","steps":false,"heartRate":true}]}
        """)!);
        Assert.Equal("en_US", Assert.Single(receipt.Observed!).Value);
        Assert.Equal((true, false), receipt.BatteryInputsFor("miwear.watch.q66.x"));
        Assert.Null(receipt.BatteryInputsFor("miwear.watch.n67"));
        Assert.Null(receipt.BatteryInputsFor(null));
        Assert.Equal((false, true), receipt.HealthInputsFor("miwear.watch.q66"));
    }

    [Fact]
    public void LiveStateReadsTheSupportMatrixAndRemainingSteps()
    {
        var live = LiveState.Parse(JsonNode.Parse("""
        {"supported":{"steps":true,"heartRate":false,"charger":true,"batteryLevel":true,
                      "batteryRaw":true,"temperature":false,"clock":false,"orientation":false},
         "notes":{"temperature":"此型号的热敏电阻换算尚未验证，暂不提供实时温度。"},
         "state":{"walkSteps":320,"walked":120,"cadence":96,"heartRate":null,"worn":true,
                  "charger":"charging","batteryRaw":2048,"temperatureC":null,"clock":null,"orientation":null}}
        """)!)!;
        Assert.True(live.Supports("steps"));
        Assert.False(live.Supports("heartRate"));
        Assert.Equal(200, live.StepsRemaining);
        Assert.True(live.Bool("worn") == true);
        Assert.Null(live.Bool("heartRate")); // null state = not reportable
        Assert.Equal(2048, live.Int("batteryRaw"));
        Assert.Equal("charging", live.Charger);
        Assert.Contains("热敏电阻", live.Note("temperature"));
        Assert.Equal("", live.Note("missing"));
        Assert.Null(LiveState.Parse(JsonNode.Parse("""{"state":{}}""")));
    }
}
