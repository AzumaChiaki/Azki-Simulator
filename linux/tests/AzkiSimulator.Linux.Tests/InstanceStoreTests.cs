using System.IO;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The instance registry is the one file this app owns that a user would notice
/// losing. The reference store refuses to overwrite data it cannot parse, holds an
/// advisory flock across the read-modify-write, and publishes through a private
/// temporary file plus rename; those behaviors are the contract under test.
/// </summary>
public class InstanceStoreTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"azki-store-{Guid.NewGuid():N}");
    private readonly InstanceStore _store;

    public InstanceStoreTests()
    {
        Directory.CreateDirectory(_root);
        _store = new InstanceStore { Root = _root };
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);

    private static InstanceRecord Record(string id, string name = "表盘") => new()
    {
        Id = id, Name = name, Model = "miwear.watch.q66", CreatedAt = 1_700_000_000,
    };

    [Fact]
    public void RecordsSurviveAReload()
    {
        _store.Save([Record("a"), Record("b", "第二台")]);
        var reloaded = new InstanceStore { Root = _root }.Load();
        Assert.Equal(["a", "b"], reloaded.Select(record => record.Id));
        Assert.Equal("第二台", reloaded[1].Name);
    }

    [Fact]
    public void MalformedDataIsNeverSilentlyReplaced()
    {
        File.WriteAllText(Path.Combine(_root, "instances.json"), "{ not json");
        Assert.ThrowsAny<Exception>(() => _store.Load());
        Assert.ThrowsAny<Exception>(() => _store.Save([Record("a")]));
        Assert.Contains("not json", File.ReadAllText(Path.Combine(_root, "instances.json")));
    }

    [Fact]
    public void SavedRegistryIsPrivateAndLeavesNoTemporaryFiles()
    {
        _store.Save([Record("a")]);
        var path = Path.Combine(_root, "instances.json");
        Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(path));
        // The only dot-file allowed is the lock; a temporary would mean a leak.
        foreach (var leftover in Directory.GetFiles(_root, ".*", SearchOption.TopDirectoryOnly))
            Assert.EndsWith(".instances.lock", leftover);
        // One link: a leftover second name would make the helper's readers reject it.
        Assert.Equal(1, UnixNative.Stat(path).Links);
    }

    [Fact]
    public void ALaterSaveReplacesTheWholeDocument()
    {
        _store.Save([Record("a"), Record("b")]);
        _store.Save([Record("c")]);
        Assert.Equal(["c"], new InstanceStore { Root = _root }.Load().Select(record => record.Id));
    }

    [Fact]
    public void RecordCountIsBounded()
    {
        var many = Enumerable.Range(0, InstanceStore.MaximumRecords + 1)
            .Select(index => Record($"instance-{index}")).ToList();
        Assert.Throws<InvalidOperationException>(() => _store.Save(many));
    }
}
