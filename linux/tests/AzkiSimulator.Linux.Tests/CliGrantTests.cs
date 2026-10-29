using System;
using System.IO;
using System.Linq;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// The one-shot CLI runs inside bwrap with only the trees its action needs mounted
/// writable. A missing grant makes the command die on a read-only tree, and an oversized
/// one hands the whole firmware library to a command that only needed one instance folder,
/// so the shape of each grant is the contract under test here.
// AZKI_* is process-global, so the suites that point it at a scratch directory share
// one collection and never run in each other's environment.
[Collection("ProcessEnvironment")]
public class CliGrantTests
{
    private const string Library = "/data/AzkiSimulator/FirmwareLibrary";
    private const string Instance = "6f1c2b3a-4d5e-4f60-8a9b-0c1d2e3f4a5b";

    [Fact]
    public void LibraryMutationsWriteOnlyTheLibrary()
    {
        foreach (var action in new[] { "library-import", "library-delete", "library-delete-all" })
        {
            var policy = Grant(action, action == "library-import" ? new[] { "--input", "/ota.bin" } : Array.Empty<string>());
            Assert.True(policy.ReadWriteTrees.SequenceEqual(new[] { Library }), $"{action} grants {string.Join(",", policy.ReadWriteTrees)}");
        }
    }

    [Fact]
    public void InstanceMutationsWriteOnlyTheirOwnFolder()
    {
        foreach (var action in new[] { "instance-settings", "instance-reset" })
        {
            var policy = Grant(action, new[] { "--instance-id", Instance });
            Assert.Equal(new[] { Path.Combine(Library, "instances", Instance) }, policy.ReadWriteTrees);
            Assert.False(policy.ReadWriteTrees.Contains(Library), $"{action} must not widen the grant to the library");
        }
    }

    [Fact]
    public void DeleteGrantsTheContainerAndProtectsTheSiblings()
    {
        // The command removes the instance folder itself, and a folder bwrap binds is a
        // mount point that cannot be unlinked from inside the namespace. So the container
        // is granted, and every sibling in it is re-bound read-only: the same mount
        // property that makes the grant necessary is what keeps it from reaching another
        // instance.
        var root = Path.Combine(Path.GetTempPath(), $"azki-grants-{Guid.NewGuid():N}");
        var library = Path.Combine(root, "FirmwareLibrary");
        var instances = Path.Combine(library, "instances");
        var sibling = Path.Combine(instances, "7a2b3c4d-5e6f-4a0b-9c8d-1e2f3a4b5c6d");
        Directory.CreateDirectory(Path.Combine(instances, Instance));
        Directory.CreateDirectory(sibling);
        try
        {
            var policy = new SandboxPolicy();
            CliGrants.Apply(policy, "instance-delete", new[] { "--instance-id", Instance }, library);
            Assert.Equal(new[] { instances }, policy.ReadWriteTrees);
            Assert.Equal(new[] { sibling }, policy.ProtectedTrees);
            Assert.DoesNotContain(Path.Combine(instances, Instance), policy.ProtectedTrees);

            var arguments = policy.BuildArguments();
            var writable = IndexOf(arguments, "--bind", instances);
            var protectedTree = IndexOf(arguments, "--ro-bind", sibling);
            Assert.True(writable >= 0 && protectedTree > writable,
                "兄弟目录的只读重绑必须排在可写绑定之后，否则可写的那个会赢");
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); } catch (IOException) { }
        }
    }

    private static int IndexOf(IReadOnlyList<string> arguments, string flag, string path)
    {
        for (var index = 0; index + 1 < arguments.Count; index++)
            if (arguments[index] == flag && arguments[index + 1] == path) return index;
        return -1;
    }

    [Fact]
    public void PrepareWritesTheInstanceFolderAndTheReservedSessionRoot()
    {
        var policy = Grant("prepare-bundled", new[]
        {
            "--profile-id", "q65-standard", "--session-root", "/tmp/azki-b-abc123def456",
            "--instance-id", Instance, "--reservation-nonce", "nonce", "--skip-oobe",
        });
        Assert.Equal(new[] { Path.Combine(Library, "instances", Instance), "/tmp/azki-b-abc123def456" }, policy.ReadWriteTrees);
    }

    [Fact]
    public void APreparationWithoutAnInstanceIdentityGrantsOnlyTheSessionRoot()
    {
        // The reference makes the instance identity optional for a preparation and
        // grants the instance folder only when it is there, so Linux follows the
        // same shape instead of refusing the command outright.
        var policy = Grant("prepare-bundled", new[]
        {
            "--profile-id", "q66-standard", "--session-root", "/tmp/azki-b-abc123def456",
        });
        Assert.Equal(new[] { "/tmp/azki-b-abc123def456" }, policy.ReadWriteTrees);
    }

    [Fact]
    public void AMalformedInstanceIdentityIsRefusedBeforeAnythingIsGranted()
    {
        Assert.Throws<BridgeRemoteError>(() => Grant("prepare-bundled", new[]
        {
            "--profile-id", "q66-standard", "--session-root", "/tmp/azki-b-abc123def456",
            "--instance-id", "../../etc",
        }));
    }

    [Fact]
    public void ReadOnlyCommandsWriteNothingButTheScratch()
    {
        var policy = Grant("library-catalog", Array.Empty<string>());
        Assert.Empty(policy.ReadWriteTrees);
        Assert.Contains(Library, policy.ReadOnlyTrees);
    }

    [Fact]
    public void TheImportInputIsExposedAsASingleReadOnlyFile()
    {
        var policy = Grant("library-import", new[] { "--input", "/downloads/ota.bin", "--model", "miwear.watch.q65" });
        Assert.Equal(new[] { "/downloads/ota.bin" }, policy.ReadOnlyFiles);
    }

    [Theory]
    [InlineData("6F1C2B3A-4D5E-4F60-8A9B-0C1D2E3F4A5B")]
    [InlineData("../../etc")]
    [InlineData("")]
    public void AnInstanceNameOutsideTheUuidShapeGrantsNothing(string id)
    {
        Assert.Throws<BridgeRemoteError>(() => Grant("instance-settings", new[] { "--instance-id", id }));
    }

    [Fact]
    public void ACommandWithoutItsRequiredIdentityIsRefusedRatherThanRunBlind()
    {
        Assert.Throws<BridgeRemoteError>(() => Grant("instance-settings", Array.Empty<string>()));
        Assert.Throws<BridgeRemoteError>(() => Grant("prepare-bundled", new[] { "--instance-id", Instance }));
    }

    [Fact]
    public void AnActionTheReferenceDoesNotAllowIsRefused()
    {
        Assert.Throws<BridgeRemoteError>(() => Grant("library-probe", new[] { "--input", "/ota.bin" }));
        Assert.Throws<BridgeRemoteError>(() => Grant("rm", new[] { "-rf", "/" }));
    }

    [Fact]
    public void CleanupBindsOnlyTheRootsThisAppOwns()
    {
        var root = Path.Combine(Path.GetTempPath(), $"azki-grants-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            var own = Path.Combine(root, "azki-b-abc123def456");
            var other = Path.Combine(root, "not-ours");
            Directory.CreateDirectory(own);
            Directory.CreateDirectory(other);
            var previous = Environment.GetEnvironmentVariable("AZKI_TMP_ROOT");
            Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", root);
            try
            {
                var policy = new SandboxPolicy { ReadOnlyTrees = { Library } };
                CliGrants.Apply(policy, "sessions-cleanup", Array.Empty<string>(), Library);
                Assert.Equal(new[] { own }, policy.ReadWriteTrees);
            }
            finally
            {
                Environment.SetEnvironmentVariable("AZKI_TMP_ROOT", previous);
            }
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void AWritableTreeIsNeverAlsoBoundReadOnly()
    {
        var policy = Grant("library-delete", new[] { "--sha256", new string('a', 64) });
        var args = policy.BuildArguments();
        Assert.True(!ContainsPair(args, "--ro-bind", Library, Library), "the library was bound read-only and writable");
        Assert.True(ContainsPair(args, "--bind", Library, Library));
    }

    private static SandboxPolicy Grant(string action, string[] argv)
    {
        var policy = new SandboxPolicy
        {
            ReadOnlyTrees = { "/app/Engine", "/app/Runtime", "/app/Firmware", Library },
        };
        CliGrants.Apply(policy, action, argv, Library);
        return policy;
    }

    private static bool ContainsPair(System.Collections.Generic.IList<string> args, string flag, string first, string second)
    {
        for (var index = 0; index + 2 < args.Count; index++)
        {
            if (args[index] == flag && args[index + 1] == first && args[index + 2] == second) return true;
        }
        return false;
    }
}
