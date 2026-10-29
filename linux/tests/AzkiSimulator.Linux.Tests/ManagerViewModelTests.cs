using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.ViewModels;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The manager window is where an instance is created. The record is written
/// before it is shown, so a store that refuses the write must not leave a
/// phantom instance in the list, and an instance that was created has to still
/// be there the next time the window opens.
/// </summary>
[Collection("ProcessEnvironment")]
public sealed class ManagerViewModelTests : IDisposable
{
    private readonly string _root;
    private readonly string? _previous;

    public ManagerViewModelTests()
    {
        _root = Path.Combine(Path.GetTempPath(), $"azki-manager-{Guid.NewGuid():N}");
        Directory.CreateDirectory(_root);
        _previous = Environment.GetEnvironmentVariable("AZKI_DATA_ROOT");
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", _root);
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("AZKI_DATA_ROOT", _previous);
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    [Fact]
    public async Task CreatingAnInstanceStoresItAndSelectsIt()
    {
        var model = new ManagerViewModel();
        var record = await model.CreateInstanceAsync("  我的表  ", "q65-standard", "q65-standard", null, null, true);
        Assert.NotNull(record);
        Assert.Equal("我的表", record!.Name);
        Assert.Equal("q65-standard", record.Model);
        Assert.Equal("q65-standard", record.SelectedProfileId);
        Assert.True(record.SkipOobe);
        Assert.Null(record.VirtualIdentity);
        Assert.Same(record, model.SelectedInstance);
        Assert.Contains("已创建实例", model.StatusText);
        Assert.Single(model.Instances);
        Assert.Single(new InstanceStore().Load());
    }

    [Fact]
    public async Task AnInstanceIsStillThereTheNextTimeTheWindowOpens()
    {
        var model = new ManagerViewModel();
        await model.CreateInstanceAsync("第一台", "q65-standard", null, "SN-1", null, false);
        var reopened = new ManagerViewModel();
        await reopened.RefreshAsync();
        var record = Assert.Single(reopened.Instances);
        Assert.Equal("第一台", record.Name);
        Assert.Equal("SN-1", record.VirtualIdentity?.Serial);
    }

    [Fact]
    public async Task ABlankNameOrModelCreatesNothing()
    {
        var model = new ManagerViewModel();
        Assert.Null(await model.CreateInstanceAsync("   ", "q65-standard", null, null, null, false));
        Assert.Null(await model.CreateInstanceAsync("名字", "  ", null, null, null, false));
        Assert.Empty(model.Instances);
    }

    [Fact]
    public async Task TwoOverlappingRefreshesDoNotDuplicateTheInstance()
    {
        // The window starts a refresh of its own, and the user can start
        // another while it runs. Overlapping refreshes must not show the same
        // instance twice: the check and the insert have to be one step.
        var model = new ManagerViewModel();
        await model.CreateInstanceAsync("并发", "q65-standard", null, null, null, false);
        var reopened = new ManagerViewModel();
        for (var round = 0; round < 20; round++)
        {
            await Task.WhenAll(Enumerable.Range(0, 16).Select(_ => reopened.RefreshAsync()));
            // The list may never hold the same instance twice, whichever
            // refresh added it.
            Assert.Equal(reopened.Instances.Count, reopened.Instances.Select(item => item.Id).Distinct().Count());
        }
    }

    [Fact]
    public async Task ARejectedWriteLeavesTheListUntouched()
    {
        // The store cannot create its root because a file is in the way, so the
        // write is refused. The list must not show an instance that was never
        // stored.
        Directory.CreateDirectory(Path.Combine(_root, "AzkiSimulator"));
        File.WriteAllText(Path.Combine(_root, "AzkiSimulator", "Instances"), "not a directory");
        var model = new ManagerViewModel();
        Assert.Null(await model.CreateInstanceAsync("写不进去", "q65-standard", null, null, null, false));
        Assert.Empty(model.Instances);
        Assert.NotEqual("已创建实例", model.StatusText);
    }
}
