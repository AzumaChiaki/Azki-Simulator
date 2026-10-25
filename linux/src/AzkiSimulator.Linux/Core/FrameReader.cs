using System.Buffers.Binary;

namespace AzkiSimulator.Linux.Core;

/// <summary>
/// Wire format: 8s "Q66FRAME" + six little-endian uint32
/// (version, sequence, width, height, stride, format), then stride*height
/// bytes of little-endian RGB565. An odd sequence means the producer is
/// writing; the reader also compares headers before/after copying (seqlock).
/// </summary>
public sealed record RawFrame(uint Sequence, int Width, int Height, int Stride, byte[] Pixels)
{
    public const int HeaderBytes = 32;
    public const int MaximumBytes = 8 * 1_024 * 1_024;

    public bool SameSize(int width, int height) => Width == width && Height == height && Stride == width * 2;

    public static RawFrame? Decode(byte[] bytes, int width, int height)
    {
        var parsed = DecodeHeader(bytes, width, height);
        if (parsed is null) return null;
        var (sequence, frameWidth, frameHeight, stride) = parsed.Value;
        if (bytes.Length != HeaderBytes + stride * frameHeight) throw new InvalidDataException("像素长度不匹配");
        var pixels = new byte[stride * frameHeight];
        Array.Copy(bytes, HeaderBytes, pixels, 0, pixels.Length);
        return new RawFrame(sequence, frameWidth, frameHeight, stride, pixels);
    }

    /// <summary>Header-only validation. Null means the producer is mid-write.</summary>
    public static (uint Sequence, int Width, int Height, int Stride)? DecodeHeader(byte[] bytes, int width, int height)
    {
        if (bytes.Length < HeaderBytes) throw new InvalidDataException("画面文件过短");
        if (System.Text.Encoding.ASCII.GetString(bytes, 0, 8) != "Q66FRAME") throw new InvalidDataException("文件头不匹配");
        uint Version = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(8));
        uint sequence = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(12));
        int frameWidth = (int)BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(16));
        int frameHeight = (int)BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(20));
        int stride = (int)BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(24));
        uint format = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(28));
        if (Version != 1 || format != 1) throw new InvalidDataException("画面版本或像素格式不受支持");
        if (sequence == 0 || (sequence & 1) != 0) return null; // producer is writing
        if (frameWidth is < 1 or > 2_048 || frameHeight is < 1 or > 2_048) throw new InvalidDataException("画面尺寸超限");
        if (stride < frameWidth * 2 || stride > 8_192 || stride % 2 != 0) throw new InvalidDataException("画面行宽无效");
        if (HeaderBytes + (long)stride * frameHeight > MaximumBytes) throw new InvalidDataException("画面长度超限");
        if (frameWidth != width || frameHeight != height || stride != width * 2)
            throw new InvalidDataException("画面尺寸或行宽与会话不一致");
        return (sequence, frameWidth, frameHeight, stride);
    }
}

/// <summary>
/// Mirrors SimulatorSessionConnection.readFrame: open through the validated
/// session directory FD (O_NOFOLLOW), header/size/ownership checks, skip when
/// the sequence is unchanged, and a pre/post header equality requirement.
/// </summary>
public sealed class FrameReader : IDisposable
{
    private const int MaximumPixelBytes = RawFrame.MaximumBytes - RawFrame.HeaderBytes;
    private readonly string _root;
    private readonly string _framebuffer;
    private readonly int _width;
    private readonly int _height;
    private uint _lastSequence;
    private bool _closed;

    public FrameReader(string sessionDirectory, SessionManifest manifest)
    {
        _root = Path.TrimEndingDirectorySeparator(sessionDirectory);
        _framebuffer = manifest.FramebufferName;
        _width = manifest.Width;
        _height = manifest.Height;
    }

    /// <summary>Returns a newer frame, or null when nothing changed / a write is in progress.</summary>
    public RawFrame? ReadNext()
    {
        if (_closed) throw new IOException("模拟器会话已断开");
        var directory = UnixNative.OpenDirectory(_root);
        using (directory)
        {
            var expectedBytes = RawFrame.HeaderBytes + _width * _height * 2;
            var stream = UnixNative.OpenRegularFile(directory, _framebuffer, expectedBytes);
            if (stream is null) return null; // ENOENT is normal before the first frame and across stop.
            using (stream)
            {
                var first = ReadExact(stream, RawFrame.HeaderBytes);
                // A seqlock in the writer's half is normal traffic, not a failure: the
                // macOS reader skips to the next deadline, so this returns null too.
                if (RawFrame.DecodeHeader(first, _width, _height) is not { } header) return null;
                if (stream.Length != RawFrame.HeaderBytes + header.Stride * header.Height)
                    throw new IOException("画面文件长度不匹配");
                if (header.Sequence == _lastSequence) return null;
                var full = ReadExact(stream, (int)stream.Length);
                var last = ReadExact(stream, RawFrame.HeaderBytes);
                if (!first.AsSpan().SequenceEqual(last) ||
                    !full.AsSpan(0, RawFrame.HeaderBytes).SequenceEqual(first)) return null;
                var frame = RawFrame.Decode(full, _width, _height);
                if (frame is null) return null;
                _lastSequence = frame.Sequence;
                return frame;
            }
        }
    }

    private static byte[] ReadExact(Stream stream, int count)
    {
        var bytes = new byte[count];
        var total = 0;
        stream.Seek(0, SeekOrigin.Begin);
        while (total < count)
        {
            var read = stream.Read(bytes.AsSpan(total, count - total));
            if (read <= 0) throw new IOException("画面已截断或无法读取");
            total += read;
        }
        return bytes;
    }

    public void Dispose() => _closed = true;
}
