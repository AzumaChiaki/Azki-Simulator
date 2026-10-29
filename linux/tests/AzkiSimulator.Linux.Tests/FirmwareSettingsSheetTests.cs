using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.ViewModels;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>The choice/row view models write straight into the shared settings
/// document, so their index-to-state mapping is the persistence contract.</summary>
public class FirmwareSettingsSheetTests
{
    [Fact]
    public void ChoiceIndexMapsToKeepValueAndReset()
    {
        var settings = InstanceFirmwareSettings.Decode(System.Text.Json.Nodes.JsonNode.Parse(
            """{"schema":1,"properties":{"persist.locale":"xx_YY"}}""")!);
        var choice = new FirmwareChoiceViewModel(settings, "语言", InstanceFirmwareSettingKeys.Locale,
            InstanceFirmwareSettingKeys.Languages, "en_US", () => "不修改（当前 en_US）");
        // The stored value is not in the option list, so it was prepended and found there;
        // a listed value (fr_FR is row 9) would map without prepending.
        Assert.Equal(1, choice.SelectedIndex);
        Assert.Equal("xx_YY", choice.Labels[1]);
        choice.SelectedIndex = 6; // en_GB: one keep row, then the prepended value shifts the list
        Assert.Equal("en_GB", settings.Properties["persist.locale"]);
        choice.SelectedIndex = 0;
        Assert.False(settings.Properties.ContainsKey("persist.locale"));
        choice.SelectedIndex = choice.Labels.Count - 1;
        Assert.True(settings.Properties.ContainsKey("persist.locale"));
        Assert.Null(settings.Properties["persist.locale"]);
    }

    [Fact]
    public void PropertyRowsFollowTheOverrideStateMachine()
    {
        var settings = InstanceFirmwareSettings.Decode(System.Text.Json.Nodes.JsonNode.Parse(
            """{"schema":1,"properties":{"persist.a":null}}""")!);
        var reset = new FirmwarePropertyRowViewModel(settings, "persist.a", "old");
        Assert.True(reset.IsReset);
        Assert.False(reset.ShowsResetButton);
        var absent = new FirmwarePropertyRowViewModel(settings, "persist.b", "x");
        Assert.True(absent.IsAbsent);
        absent.EditCommand.Execute(null);
        Assert.True(absent.IsOverride);
        Assert.Equal("x", settings.Properties["persist.b"]);
        absent.ValueText = "edited";
        Assert.Equal("edited", settings.Properties["persist.b"]);
        absent.UndoCommand.Execute(null);
        Assert.True(absent.IsAbsent);
        Assert.False(settings.Properties.ContainsKey("persist.b"));
    }

    [Fact]
    public void AddPropertyValidatesAndRebuildsRows()
    {
        var record = new InstanceRecord { Id = Guid.NewGuid().ToString(), Name = "n", Model = "m", CreatedAt = 0 };
        // The add/filter path never touches the CLI runner, so a null one is honest here.
        var sheet = new FirmwareSettingsViewModel(record, null!, () => Task.CompletedTask, () => false);
        sheet.NewKey = "bad.key";
        sheet.NewValue = "v";
        sheet.AddCommand.Execute(null);
        Assert.Contains("persist.", sheet.Message);
        Assert.Empty(sheet.Rows);
        sheet.NewKey = "persist.demo";
        sheet.NewValue = "7";
        sheet.AddCommand.Execute(null);
        Assert.True(sheet.HasMessage == false);
        Assert.Single(sheet.Rows);
        sheet.Filter = "nomatch";
        Assert.Empty(sheet.Rows);
        sheet.Filter = "DEMO";
        Assert.Single(sheet.Rows);
    }
}
