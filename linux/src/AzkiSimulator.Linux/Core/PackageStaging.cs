using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Stages a package into the session's incoming folder the way SimulatorPackageImport does.
/// The source must be a private regular file no larger than the helper's bound, the copy is
/// streaming and hashed in one pass, the staged name is created exclusively at 0600 inside a
/// 0700 incoming folder, and the source plus both directories are re-checked against the
/// identities captured before the first byte moved. The helper rehashes the bytes itself, so
/// these checks protect against a swapped file rather than being the trust anchor.
/// </summary>
public static class PackageStaging
{
    public const long MaximumBytes = 32L * 1024 * 1024;
    private const int Chunk = 65_536;

    public static (string FileName, string Sha256, long Bytes) Stage(string sessionDirectory, string sourcePath,
        Guid identifier, Action? afterChunk = null)
    {
        var fileName = identifier.ToString() + ".bin";
        var source = UnixNative.OpenNoFollowAll(sourcePath, UnixNative.O_RDONLY | UnixNative.O_NONBLOCK);
        var before = SourceInfo(UnixNative.FStat(source), sourcePath);
        using var reader = new FileStream(source, FileAccess.Read);
        using var root = UnixNative.OpenNoFollowAll(sessionDirectory, UnixNative.O_RDONLY | UnixNative.O_DIRECTORY);
        Private(root, "会话目录");
        var rootIdentity = UnixNative.FStat(root);
        // The reference creates the incoming folder with mkdirat on the opened
        // root, so the name is resolved through the validated handle rather
        // than through a path that could have been replaced meanwhile.
        UnixNative.MkdirPrivate(root, "incoming");
        using var incoming = UnixNative.OpenAt(root, "incoming",
            UnixNative.O_RDONLY | UnixNative.O_DIRECTORY | UnixNative.O_NOFOLLOW | UnixNative.O_CLOEXEC);
        Private(incoming, "导入目录");
        var incomingIdentity = UnixNative.FStat(incoming);        using var output = OpenStaged(incoming, fileName);
        var outputIdentity = UnixNative.FStat(output);
        OutputInfo(outputIdentity, 0);

        using var stream = new FileStream(output, FileAccess.Write);
        using var hasher = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[Chunk];
        var count = 0L;
        var completed = false;
        try
        {
            while (true)
            {
                var length = reader.Read(buffer, 0, buffer.Length);
                if (length == 0) break;
                count += length;
                if (count > MaximumBytes) throw new IOException("导入文件超过 32 MiB");
                hasher.AppendData(buffer, 0, length);
                stream.Write(buffer, 0, length);
                afterChunk?.Invoke();
            }
            stream.Flush(flushToDisk: true);
            if (!SameSource(before, UnixNative.FStat(reader.SafeFileHandle)) || count != before.Size)
                throw new IOException("源文件在复制过程中发生变化");
            // The name may have been pointed at another inode while the copy ran.
            using (var reopened = UnixNative.OpenNoFollowAll(sourcePath, UnixNative.O_RDONLY | UnixNative.O_NONBLOCK))
            {
                if (!SameSource(before, UnixNative.FStat(reopened))) throw new IOException("源文件路径在复制过程中发生变化");
            }
            var written = UnixNative.FStat(output);
            if (!SameIdentity(outputIdentity, written) || !UnixNative.TryStatAt(incoming, fileName, out var visible)
                || !SameIdentity(outputIdentity, visible))
                throw new IOException("导入文件在复制过程中被替换");
            OutputInfo(written, count);
            Recheck(root, rootIdentity, "会话目录");
            Recheck(incoming, incomingIdentity, "导入目录");
            completed = true;
        }
        finally
        {
            if (!completed) UnixNative.UnlinkOwn(incoming, fileName, outputIdentity);
        }
        UnixNative.fsync((int)incoming.DangerousGetHandle());
        UnixNative.fsync((int)root.DangerousGetHandle());
        GC.KeepAlive(incoming);
        GC.KeepAlive(root);
        return (fileName, Convert.ToHexString(hasher.GetHashAndReset()).ToLowerInvariant(), count);
    }

    private static SafeFileHandle OpenStaged(SafeFileHandle incoming, string fileName)
    {
        try
        {
            return UnixNative.OpenAt(incoming, fileName, UnixNative.O_WRONLY | UnixNative.O_CREAT | UnixNative.O_EXCL
                | UnixNative.O_NOFOLLOW | UnixNative.O_CLOEXEC, 0x180);
        }
        catch (IOException)
        {
            throw new IOException("无法独占创建导入文件");
        }
    }

    private static void Recheck(SafeFileHandle handle, StatResult expected, string label)
    {
        Private(handle, label);
        if (!SameIdentity(expected, UnixNative.FStat(handle))) throw new IOException($"{label}被替换");
    }

    private static void Private(SafeFileHandle handle, string label)
    {
        var value = UnixNative.FStat(handle);
        if (!UnixNative.IsDirectory(value.Mode) || value.Uid != UnixNative.geteuid()
            || UnixNative.ModeBits(value.Mode) != 0x1C0)
            throw new IOException($"{label}须属于当前用户且权限为 0700");
    }

    private static StatResult SourceInfo(StatResult value, string path)
    {
        if (!UnixNative.IsRegular(value.Mode) || value.Links != 1
            || (value.Uid != UnixNative.geteuid() && value.Uid != 0)
            || (value.Mode & UnixNative.WorldWritableBits) != 0
            || value.Size < 0 || value.Size > MaximumBytes)
            throw new IOException($"{path} 必须为不超过 32 MiB 的普通文件，且所有者、链接数和写入权限须符合要求");
        return value;
    }

    private static void OutputInfo(StatResult value, long size)
    {
        if (!UnixNative.IsRegular(value.Mode) || value.Links != 1 || value.Uid != UnixNative.geteuid()
            || UnixNative.ModeBits(value.Mode) != 0x180 || value.Size != size)
            throw new IOException("导入文件属性发生变化");
    }

    private static bool SameIdentity(StatResult left, StatResult right) =>
        left.Device == right.Device && left.Inode == right.Inode;

    private static bool SameSource(StatResult left, StatResult right) =>
        SameIdentity(left, right) && left.Size == right.Size && left.Mode == right.Mode
        && left.Uid == right.Uid && left.Links == right.Links;
}
