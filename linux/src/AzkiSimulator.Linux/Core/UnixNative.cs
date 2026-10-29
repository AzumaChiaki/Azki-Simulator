using System.Buffers.Binary;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace AzkiSimulator.Linux.Core;

/// <summary>glibc calls the supervisor needs for sandbox-equivalent checks.</summary>
internal static class UnixNative
{
    [DllImport("libc", SetLastError = true)]
    public static extern uint getuid();

    [DllImport("libc", SetLastError = true)]
    public static extern uint geteuid();

    [DllImport("libc", SetLastError = true)]
    public static extern int getpgid(int pid);

    [DllImport("libc", SetLastError = true)]
    public static extern int killpg(int pgrp, int sig);

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern int link(string source, string destination);

    [DllImport("libc", SetLastError = true)]
    private static extern int lstat(string path, byte[] buffer);

    [DllImport("libc", SetLastError = true)]
    private static extern int getsockopt(int socket, int level, int option, byte[] value, ref int length);

    public const int SolSocket = 1;
    // SO_PEERCRED, struct ucred {pid, uid, gid}. It is 17 on Linux, not the 16
    // a BSD LOCAL_PEERCRED might suggest: 16 is SO_BSDCOMPAT, which succeeds and
    // hands back four zero bytes, so the old value failed the credential check
    // without ever reporting an error.
    public const int SoPeercred = 17;

    public static void HardLink(string source, string destination)
    {
        if (link(source, destination) != 0)
            throw new IOException($"link failed: {Marshal.GetLastPInvokeError()}", Marshal.GetLastPInvokeError());
    }

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern int renameat2(int oldDirectoryFd, string oldPath, int newDirectoryFd, string newPath, uint flags);

    public const int AtFdcwd = -100;
    public const uint RenameNoReplace = 1; // RENAME_NOREPLACE
    public const int Exist = 17; // EEXIST

    /// <summary>Atomic exclusive publication, the same idiom the helper and the
    /// reference broker use (renameat2 RENAME_NOREPLACE / renamex_np RENAME_EXCL).
    /// A hard link plus unlink would leave the destination with st_nlink of 2 for
    /// as long as the temporary name survives, and the helper reads broker records
    /// with a hard "non-linked regular file" check while it polls every 25 ms.</summary>
    public static bool TryRenameExclusive(string source, string destination, out int errorCode)
    {
        if (renameat2(AtFdcwd, source, AtFdcwd, destination, RenameNoReplace) == 0)
        {
            errorCode = 0;
            return true;
        }
        errorCode = Marshal.GetLastPInvokeError();
        return false;
    }

    public static void RenameExclusive(string source, string destination)
    {
        if (!TryRenameExclusive(source, destination, out var error))
            throw new IOException($"renameat2 失败 (errno {error})", error);
    }

    [DllImport("libc", SetLastError = true)]
    public static extern int fsync(int fd);

    /// <summary>Flushes a directory so a completed rename survives a crash; the
    /// reference store syncs the directory after every save, not just the file.</summary>
    public static void SyncDirectory(string path)
    {
        using var handle = OpenDirectory(path);
        if (fsync(handle.DangerousGetHandle().ToInt32()) != 0)
            throw new IOException($"目录同步失败 {path}", Marshal.GetLastPInvokeError());
    }

    /// <summary>Atomically publishes one small private record, the way the reference
    /// store does: 0600 from creation (a write followed by chmod is readable under the
    /// default umask in between), fsynced, renamed (exclusively when the file did not
    /// exist), then the directory is flushed.</summary>
    public static void PublishRecord(string directory, string prefix, string name, string content, bool mayReplace)
    {
        var temporary = Path.Combine(directory, $".{prefix}-{Guid.NewGuid():N}.tmp");
        var target = Path.Combine(directory, name);
        try
        {
            using (var stream = CreatePrivateExclusive(temporary))
            {
                var payload = System.Text.Encoding.UTF8.GetBytes(content);
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }
            var replace = mayReplace && File.Exists(target);
            if (replace) File.Move(temporary, target, overwrite: true);
            else RenameExclusive(temporary, target);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
        SyncDirectory(directory);
    }

    [DllImport("libc", SetLastError = true)]
    public static extern int flock(int fd, int operation);

    public const int LockEx = 2, LockNb = 4;

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern int open(string path, int flags, int mode = 0);

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern int openat(int directoryFd, string path, int flags, int mode = 0);

    // asm-generic values, octal as spelled in <fcntl.h>: O_DIRECTORY 0200000,
    // O_NOFOLLOW 0400000, O_CLOEXEC 02000000. 0x40000 would be O_NOATIME, which
    // silently follows symlinks, so these hex spellings are checked by a test.
    public const int O_RDONLY = 0;
    public const int O_WRONLY = 0x1;
    public const int O_CREAT = 0x40;
    public const int O_EXCL = 0x80;
    public const int O_NONBLOCK = 0x800;
    public const int O_DIRECTORY = 0x10000;
    public const int O_NOFOLLOW = 0x20000;
    public const int O_CLOEXEC = 0x80000;

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi, EntryPoint = "mkdirat")]
    private static extern int mkdirat(int directoryFd, string path, int mode);

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern int unlinkat(int directoryFd, string path, int flags);

    /// <summary>Opens a directory handle relative to an already validated parent, so the
    /// name is resolved by the kernel inside that directory and cannot escape it.</summary>
    public static SafeFileHandle OpenAt(SafeFileHandle directory, string name, int flags, int mode = 0)
    {
        var fd = openat((int)directory.DangerousGetHandle(), name, flags, mode);
        GC.KeepAlive(directory);
        if (fd < 0) throw new IOException($"无法打开 {name}", Marshal.GetLastPInvokeError());
        return new SafeFileHandle(fd, ownsHandle: true);
    }

    /// <summary>Opens one path without following a symlink at any level: every component is
    /// resolved with openat plus O_NOFOLLOW from the root down, which is how the helper reads
    /// the same file. A plain open() would follow an intermediate link.</summary>
    public static SafeFileHandle OpenNoFollowAll(string path, int flags)
    {
        if (!Path.IsPathRooted(path)) throw new IOException($"路径必须绝对：{path}");
        var handle = new SafeFileHandle(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), ownsHandle: true);
        if (handle.IsInvalid) throw new IOException("无法打开根目录", Marshal.GetLastPInvokeError());
        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0) return handle;
        for (var index = 0; index < parts.Length; index++)
        {
            var last = index == parts.Length - 1;
            var next = OpenAt(handle, parts[index],
                (last ? flags : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY));
            handle.Dispose();
            handle = next;
        }
        return handle;
    }

    /// <summary>Creates a private directory relative to an already validated directory
    /// handle, with an exact mode: Directory.CreateDirectory applies the process umask
    /// to 0777, which would leave a 0755 folder where the reference demands 0700. The
    /// reference creates it the same way (mkdirat on the opened root), so the name cannot
    /// be resolved through a path that was replaced in between.</summary>
    public static void MkdirPrivate(SafeFileHandle directory, string name)
    {
        if (name.Contains('/') || name == "." || name == "..") throw new IOException($"不安全的目录名 {name}");
        if (mkdirat((int)directory.DangerousGetHandle(), name, 0x1C0) != 0)
        {
            var error = Marshal.GetLastPInvokeError();
            if (error != 17) throw new IOException($"无法创建私有目录 {name}", error); // EEXIST: it is checked below.
        }
        GC.KeepAlive(directory);
    }

    /// <summary>Removes a name only while it still refers to the given identity, so a failed
    /// write never unlinks a file somebody else published under the same name.</summary>
    public static void UnlinkOwn(SafeFileHandle directory, string name, StatResult identity)
    {
        if (!TryStatAt(directory, name, out var visible) || visible.Device != identity.Device || visible.Inode != identity.Inode)
            return;
        if (unlinkat((int)directory.DangerousGetHandle(), name, 0) != 0)
            throw new IOException($"无法清理 {name}", Marshal.GetLastPInvokeError());
        GC.KeepAlive(directory);
    }

    public static bool TryStatAt(SafeFileHandle directory, string name, out StatResult result)
    {
        result = default;
        try
        {
            using var handle = OpenAt(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            result = FStat(handle);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
    }

    public static StatResult FStat(SafeFileHandle handle)
    {
        var result = FStat((int)handle.DangerousGetHandle(), "描述符");
        GC.KeepAlive(handle);
        return result;
    }

    /// <summary>Creates a 0600 file that must not exist yet. File.Create plus a later
    /// chmod would leave a window where the file is world-readable, which the helper
    /// treats as an unsafe reservation.</summary>
    public static FileStream CreatePrivateExclusive(string path)
    {
        var fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0x180);
        if (fd < 0) throw new IOException($"无法以私有权限创建 {path}", Marshal.GetLastPInvokeError());
        return new FileStream(new SafeFileHandle(fd, ownsHandle: true), FileAccess.Write);
    }

    // S_IFMT 0170000, S_IFSOCK 0140000, S_IFDIR 040000, S_IFREG 0100000, S_IFLNK 0120000.
    public const uint NodeTypeMask = 0xF000;
    public const uint SocketType = 0xC000;
    public const uint DirectoryType = 0x4000;
    public const uint RegularType = 0x8000;
    public const uint SymlinkType = 0xA000;

    /// <summary>Group-writable or world-writable bits (0022).</summary>
    public const uint WorldWritableBits = 0x12;

    /// <summary>Any access for group or other (0077).</summary>
    public const uint GroupOtherAccessBits = 0x3F;

    public static SafeFileHandle OpenDirectory(string path)
    {
        var fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) throw new IOException($"无法打开目录 {path}", Marshal.GetLastPInvokeError());
        return new SafeFileHandle(fd, ownsHandle: true);
    }

    /// <summary>Opens a regular file inside an already validated directory FD.
    /// Null means ENOENT, which is normal before a file is first published.</summary>
    public static FileStream? OpenRegularFile(SafeFileHandle directory, string name, int limit)
    {
        if (name.Contains('/') || name == "." || name == "..") throw new IOException($"不安全的文件名 {name}");
        var fd = openat((int)directory.DangerousGetHandle(), name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        GC.KeepAlive(directory);
        if (fd < 0)
        {
            var error = Marshal.GetLastPInvokeError();
            if (error == 2) return null; // ENOENT: missing frame is normal before first publication.
            throw new IOException($"无法读取 {name}", error);
        }
        var handle = new SafeFileHandle(fd, ownsHandle: true);
        var stream = new FileStream(handle, FileAccess.Read);
        try
        {
            var stat = new byte[144];
            if (fstat(stream.SafeFileHandle.DangerousGetHandle().ToInt32(), stat) != 0) throw new IOException($"无法核验 {name}");
            var info = ParseStat(stat);
            if (!IsRegular(info.Mode) || info.Uid != getuid() || info.Links != 1
                || (info.Mode & WorldWritableBits) != 0 || info.Size < 0 || info.Size > limit)
                throw new IOException($"{name} 类型、所有权或大小不符合要求");
            return stream;
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }


    /// <summary>Reads one small broker record the way the helper and the reference
    /// broker read theirs: through an already validated directory handle, no symlink,
    /// regular, owned, exactly one link, within the size bound, and identical across
    /// the read. Null means the record has not been published yet.</summary>
    public static string? ReadRecord(SafeFileHandle directory, string name, int limit)
    {
        using var stream = OpenRegularFile(directory, name, limit);
        if (stream is null) return null;
        var handle = stream.SafeFileHandle.DangerousGetHandle().ToInt32();
        var before = FStat(handle, name);
        var buffer = new byte[limit + 1];
        var total = 0;
        while (total < buffer.Length)
        {
            var read = stream.Read(buffer, total, buffer.Length - total);
            if (read <= 0) break;
            total += read;
        }
        var after = FStat(handle, name);
        if (total > limit || before != after)
            throw new IOException($"{name} 在读取中被替换或改变");
        return System.Text.Encoding.UTF8.GetString(buffer, 0, total);
    }

    private static StatResult FStat(int fd, string name)
    {
        var buffer = new byte[144];
        if (fstat(fd, buffer) != 0) throw new IOException($"无法核验 {name}", Marshal.GetLastPInvokeError());
        return ParseStat(buffer);
    }

    [DllImport("libc", SetLastError = true)]
    private static extern int fstat(int fd, byte[] buffer);

    // These offsets are glibc's x86-64 struct stat, confirmed with offsetof(). Other ABIs
    // reorder st_mode and st_nlink, where guessing would silently validate the wrong bytes.
    private static readonly bool LayoutVerified = RuntimeInformation.OSArchitecture == Architecture.X64;

    /// <summary>Checked once at startup instead of deep inside the frame loop.</summary>
    public static bool SupportedAbi => LayoutVerified;

    private static StatResult ParseStat(byte[] buffer)
    {
        if (!LayoutVerified)
            throw new PlatformNotSupportedException($"暂不支持 {RuntimeInformation.OSArchitecture} 主机架构的文件属性校验");
        var bytes = buffer.AsSpan();
        return new StatResult(
            BinaryPrimitives.ReadInt64LittleEndian(bytes.Slice(0)),   // st_dev
            BinaryPrimitives.ReadInt64LittleEndian(bytes.Slice(8)),   // st_ino
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(24)), // st_mode
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(28)), // st_uid
            BinaryPrimitives.ReadInt64LittleEndian(bytes.Slice(16)),  // st_nlink
            BinaryPrimitives.ReadInt64LittleEndian(bytes.Slice(48))); // st_size
    }

    public static StatResult Stat(string path)
    {
        if (!TryStat(path, out var result, out var error)) throw error!;
        return result;
    }

    /// <summary>lstat without turning a vanished file into an exception; a missing
    /// control socket is normal, while an unreadable existing entry must still fail.</summary>
    public static bool TryStat(string path, out StatResult result, out IOException? error)
    {
        var buffer = new byte[144];
        result = default;
        error = null;
        if (lstat(path, buffer) != 0)
        {
            var code = Marshal.GetLastPInvokeError();
            if (code == 2) return false; // ENOENT
            error = new IOException($"lstat failed for {path}: {code}", code);
            return true;
        }
        try { result = ParseStat(buffer); }
        catch (Exception failure) when (failure is IOException or PlatformNotSupportedException)
        {
            error = new IOException($"无法解析 {path} 的文件属性", failure);
            return true;
        }
        return true;
    }

    public static (int Pid, uint Uid, uint Gid) PeerCredentials(nint socketHandle)
    {
        var buffer = new byte[12];
        var length = buffer.Length;
        if (getsockopt((int)socketHandle, SolSocket, SoPeercred, buffer, ref length) != 0 || length < 12)
            throw new IOException($"SO_PEERCRED unavailable: {Marshal.GetLastPInvokeError()}");
        return (BitConverter.ToInt32(buffer, 0), BitConverter.ToUInt32(buffer, 4), BitConverter.ToUInt32(buffer, 8));
    }

    public static bool IsSocket(uint mode) => (mode & NodeTypeMask) == SocketType;
    public static bool IsDirectory(uint mode) => (mode & NodeTypeMask) == DirectoryType;
    public static bool IsRegular(uint mode) => (mode & NodeTypeMask) == RegularType;
    public static bool IsSymlink(uint mode) => (mode & NodeTypeMask) == SymlinkType;
    public static int ModeBits(uint mode) => (int)(mode & 0xFFF);
}

public readonly record struct StatResult(long Device, long Inode, uint Mode, uint Uid, long Links, long Size);
