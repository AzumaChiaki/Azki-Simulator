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
    public const int SoPeercred = 16; // SO_PEERCRED, struct ucred {pid, uid, gid}

    public static void HardLink(string source, string destination)
    {
        if (link(source, destination) != 0)
            throw new IOException($"link failed: {Marshal.GetLastPInvokeError()}", Marshal.GetLastPInvokeError());
    }

    public static bool TryHardLink(string source, string destination, out int errorCode)
    {
        if (link(source, destination) == 0)
        {
            errorCode = 0;
            return true;
        }
        errorCode = Marshal.GetLastPInvokeError();
        return false;
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
    public const int O_DIRECTORY = 0x10000;
    public const int O_NOFOLLOW = 0x20000;
    public const int O_CLOEXEC = 0x80000;

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
