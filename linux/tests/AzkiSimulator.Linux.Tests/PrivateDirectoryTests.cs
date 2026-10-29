using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The session helper refuses a run directory that is not mode 0700, and the
/// guest run directory is normally created as an ancestor of its "tmp" child.
/// Directory.CreateDirectory fills that ancestor in at the umask default, so
/// without walking the chain the start failed with "A same-user mode-0700
/// directory is required" before any core process existed.
/// </summary>
public sealed class PrivateDirectoryTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"azki-private-{Guid.NewGuid():N}");

    [Fact]
    public void MakesEveryAncestorItCreatesPrivate()
    {
        var leaf = Path.Combine(_root, "session", "m-0123456789ab", "tmp");
        DataLocations.EnsurePrivateDirectory(leaf);

        foreach (var created in new[] { _root, Path.Combine(_root, "session"),
                                         Path.Combine(_root, "session", "m-0123456789ab"), leaf })
            Assert.Equal(0b111_000_000, (int)UnixNative.Stat(created).Mode & 0b111_111_111);
    }

    [Fact]
    public void TightensAnExistingDirectoryOwnedByTheCaller()
    {
        var leaf = Path.Combine(_root, "open");
        Directory.CreateDirectory(leaf);
        File.SetUnixFileMode(leaf, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                                  UnixFileMode.GroupRead | UnixFileMode.OtherRead);

        DataLocations.EnsurePrivateDirectory(leaf);

        Assert.Equal(0b111_000_000, (int)UnixNative.Stat(leaf).Mode & 0b111_111_111);
    }

    [Fact]
    public void LeavesAncestorsThatAlreadyExistAlone()
    {
        // The data root is shared with the rest of the user's desktop state; a
        // helper that promises one private directory must not re-modal its parents.
        var existing = Path.Combine(_root, "share");
        Directory.CreateDirectory(existing);
        File.SetUnixFileMode(existing, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                                       UnixFileMode.GroupRead | UnixFileMode.GroupWrite | UnixFileMode.GroupExecute |
                                       UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute);

        DataLocations.EnsurePrivateDirectory(Path.Combine(existing, "instance", "run"));

        Assert.Equal(0b111_111_111, (int)UnixNative.Stat(existing).Mode & 0b111_111_111);
        Assert.Equal(0b111_000_000,
            (int)UnixNative.Stat(Path.Combine(existing, "instance", "run")).Mode & 0b111_111_111);
    }

    public void Dispose()
    {
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }
}
