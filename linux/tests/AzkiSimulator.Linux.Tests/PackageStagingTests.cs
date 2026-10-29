using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using AzkiSimulator.Linux.Core;
using Xunit;

namespace AzkiSimulator.Linux.Tests;

/// <summary>
/// Staging is the one place where the desktop app writes into a session directory that a
/// sandboxed helper later reads and trusts by name. The reference copy is a bounded streaming
/// transfer that checks the source, the two directories and the staged inode around it, so
/// those properties are what this suite pins down.
/// </summary>
public class PackageStagingTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"azki-stage-{Guid.NewGuid():N}");
    private readonly string _session;
    private readonly string _source;
    private readonly Guid _identifier = Guid.NewGuid();

    public PackageStagingTests()
    {
        Directory.CreateDirectory(_root);
        _session = Path.Combine(_root, "session");
        DataLocations.EnsurePrivateDirectory(_session);
        _source = Path.Combine(_root, "package.bin");
        File.WriteAllBytes(_source, Payload(4096));
        File.SetUnixFileMode(_source, UnixFileMode.UserRead | UnixFileMode.UserWrite);
    }

    public void Dispose()
    {
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    private static byte[] Payload(int length)
    {
        var bytes = new byte[length];
        for (var index = 0; index < length; index++) bytes[index] = (byte)(index % 251);
        return bytes;
    }

    private string StagedPath => Path.Combine(_session, "incoming", _identifier + ".bin");

    [Fact]
    public void StagesPrivatelyAndReportsTheDigestOfTheBytes()
    {
        var (fileName, sha, bytes) = PackageStaging.Stage(_session, _source, _identifier);
        Assert.Equal(_identifier + ".bin", fileName);
        Assert.Equal(4096, bytes);
        Assert.Equal(Convert.ToHexString(SHA256.HashData(Payload(4096))).ToLowerInvariant(), sha);
        Assert.Equal(File.ReadAllBytes(StagedPath), Payload(4096));
        Assert.Equal(0x180, Mode(StagedPath));
        Assert.Equal(0x1C0, Mode(Path.Combine(_session, "incoming")));
    }

    [Fact]
    public void AnExistingNameIsNeverOverwritten()
    {
        DataLocations.EnsurePrivateDirectory(Path.Combine(_session, "incoming"));
        File.WriteAllText(StagedPath, "someone else's");
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier));
        Assert.Contains("独占", failure.Message);
        Assert.Equal("someone else's", File.ReadAllText(StagedPath));
    }

    [Fact]
    public void AGroupWritableSourceIsRefused()
    {
        File.SetUnixFileMode(_source, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupWrite);
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier));
        Assert.Contains("普通文件", failure.Message);
        Assert.False(File.Exists(StagedPath));
    }

    [Fact]
    public void ASymbolicLinkSourceIsRefused()
    {
        var link = Path.Combine(_root, "linked.bin");
        File.Delete(link);
        File.CreateSymbolicLink(link, _source);
        Assert.Throws<IOException>(() => PackageStaging.Stage(_session, link, _identifier));
        Assert.False(Directory.Exists(Path.Combine(_session, "incoming")));
    }

    [Fact]
    public void AnOversizedSourceIsRefusedBeforeAnyBytesMove()
    {
        using (var sparse = File.Create(_source)) sparse.SetLength(PackageStaging.MaximumBytes + 1);
        File.SetUnixFileMode(_source, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier));
        Assert.Contains("32 MiB", failure.Message);
        Assert.False(File.Exists(StagedPath));
    }

    [Fact]
    public void AWorldReadableSessionIsRefused()
    {
        File.SetUnixFileMode(_session, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute
            | UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier));
        Assert.Contains("0700", failure.Message);
    }

    [Fact]
    public void AFailedCopyLeavesNoPartialPackage()
    {
        var failure = Assert.Throws<InvalidOperationException>(() => PackageStaging.Stage(_session, _source, _identifier,
            () => throw new InvalidOperationException("写入中断")));
        Assert.Equal("写入中断", failure.Message);
        Assert.False(File.Exists(StagedPath));
    }

    /// <summary>The swap is caught by the identity check, and the cleanup must then leave
    /// the impostor alone: unlinking by name would delete a file this staging never made.</summary>
    [Fact]
    public void AFailedCopyDoesNotRemoveSomebodyElsesFile()
    {
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier, () =>
        {
            File.Delete(StagedPath);
            File.WriteAllText(StagedPath, "impostor");
        }));
        Assert.Contains("被替换", failure.Message);
        Assert.Equal("impostor", File.ReadAllText(StagedPath));
    }

    /// <summary>Renaming another file over the source name drops the open inode's link count
    /// to zero, which the identity comparison against the descriptor catches; the pathname is
    /// then reopened as a second check. Either way a receipt must not describe bytes that are
    /// no longer behind the name the user picked.</summary>
    [Fact]
    public void AReplacedSourceMidCopyIsDetected()
    {
        var failure = Assert.Throws<IOException>(() => PackageStaging.Stage(_session, _source, _identifier, () =>
        {
            var other = Path.Combine(_root, "other.bin");
            File.WriteAllBytes(other, Payload(2048));
            File.SetUnixFileMode(other, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            File.Move(other, _source, overwrite: true);
        }));
        Assert.Contains("复制过程中发生变化", failure.Message);
        Assert.False(File.Exists(StagedPath));
    }

    /// <summary>The mode bits as the kernel sees them, because File.GetUnixFileMode has no
    /// numeric form and a wrong octal literal here would silently pass.</summary>
    private static int Mode(string path)
    {
        var stat = UnixNative.Stat(path);
        return (int)(stat.Mode & 0xFFF);
    }

    [Fact]
    public void TheIncomingFolderIsCreatedRelativeToTheValidatedHandle()
    {
        // The reference creates the folder with mkdirat on the opened root. A
        // path-based mkdir resolves the name a second time, so a session path
        // that was replaced in between would create the folder somewhere else.
        var reference = RepositoryFile("Sources/AzkiSimulator/Core/SimulatorPackageImport.swift");
        Assert.Contains("mkdirat(root, \"incoming\", 0o700)", reference);
        var staging = RepositoryFile("linux/src/AzkiSimulator.Linux/Core/PackageStaging.cs");
        Assert.Contains("MkdirPrivate(root, \"incoming\")", staging);
        Assert.DoesNotContain("MkdirPrivate(Path.Combine(sessionDirectory", staging);
        Assert.Contains("EntryPoint = \"mkdirat\"", RepositoryFile("linux/src/AzkiSimulator.Linux/Core/UnixNative.cs"));
    }

    private static string RepositoryFile(string relativePath)
    {
        for (var probe = new DirectoryInfo(AppContext.BaseDirectory); probe is not null; probe = probe.Parent)
        {
            var candidate = Path.Combine(probe.FullName, relativePath);
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }
        throw new FileNotFoundException($"Repository file not found: {relativePath}");
    }
}
