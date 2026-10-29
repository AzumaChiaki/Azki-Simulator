using System.Diagnostics;
using System.Text;
using AzkiSimulator.Linux.Core;

namespace AzkiSimulator.Linux.Services;

/// <summary>
/// Records this instance's framebuffer only — the contract of
/// SimulatorRecordingModel.swift: one sample in flight, unchanged frames are
/// re-sampled so a static watchface keeps its real duration, a failed encode
/// keeps the private staging file for recovery, and publishing never replaces
/// an existing file. The encoder itself is the optional external dependency
/// ffmpeg (rawvideo rgb565le to H.264 mp4); nothing here encodes by hand.
/// State is plain fields; Changed fires off the UI thread and DeviceViewModel
/// marshals it, the same way the log pump does.
/// </summary>
public sealed class RecordingModel
{
    private static readonly TimeSpan SampleInterval = TimeSpan.FromMilliseconds(33);
    internal const int Framerate = 30;

    private readonly string? _outputOverride;
    private readonly string? _ffmpegOverride;
    private readonly object _gate = new();
    private CancellationTokenSource? _cancellation;
    private Task? _runner;
    private volatile bool _stopRequested;

    public RecordingModel(string? outputDirectory = null, string? ffmpegPath = null)
    {
        _outputOverride = outputDirectory;
        _ffmpegOverride = ffmpegPath;
    }

    public bool IsRecording { get; private set; }
    public bool IsFinishing { get; private set; }
    public TimeSpan Elapsed { get; private set; }
    public string? SavedFile { get; private set; }
    public string? RecoveryFile { get; private set; }
    public string? ErrorMessage { get; private set; }

    /// <summary>Raised from the sample thread; subscribers must marshal to the UI.</summary>
    public event Action? Changed;

    /// <summary>Mirrors the Swift guard: a missing first frame is a refusal, not a crash.</summary>
    public bool Start(string instanceName, Func<RawFrame?> frameSource)
    {
        lock (_gate)
        {
            if (IsRecording || IsFinishing || _runner is not null || _stopRequested) return false;
            ErrorMessage = null;
            SavedFile = null;
            RecoveryFile = null;
            Elapsed = TimeSpan.Zero;
            var first = frameSource();
            if (first is null)
            {
                ErrorMessage = "请等待设备启动并显示画面后录制";
                Changed?.Invoke();
                return false;
            }
            FfmpegEncoder encoder;
            try
            {
                encoder = FfmpegEncoder.Launch(_ffmpegOverride ?? "ffmpeg", first.Width, first.Height,
                    Framerate, _outputOverride ?? DefaultOutputDirectory(), instanceName);
            }
            catch (Exception error) when (error is IOException or InvalidOperationException)
            {
                ErrorMessage = "无法开始录屏：" + error.Message;
                Changed?.Invoke();
                return false;
            }
            var token = (_cancellation = new CancellationTokenSource()).Token;
            IsRecording = true;
            var started = MonotonicClock.Now;
            var width = first.Width;
            var height = first.Height;
            var lastReport = started;
            _runner = Task.Run(async () =>
            {
                string? interrupted = null;
                while (!token.IsCancellationRequested)
                {
                    var frame = frameSource();
                    if (frame is null)
                    {
                        interrupted = "设备画面已停止，录屏结束。";
                        break;
                    }
                    // FrameReader pins geometry to the session manifest, but a session
                    // swap would change the manifest, and appending two sizes to one
                    // ffmpeg pipe would corrupt the stream — stop instead.
                    if (frame.Width != width || frame.Height != height)
                    {
                        interrupted = "设备画面尺寸已改变，原尺寸录像已结束。";
                        break;
                    }
                    try
                    {
                        await encoder.WriteAsync(frame.Pixels, token);
                        Elapsed = MonotonicClock.Now - started;
                    }
                    catch (OperationCanceledException) { break; }
                    catch (Exception error) when (error is IOException or InvalidOperationException)
                    {
                        interrupted = "录屏已结束：" + error.Message;
                        break;
                    }
                    var now = MonotonicClock.Now;
                    if (now - lastReport >= TimeSpan.FromSeconds(1))
                    {
                        lastReport = now;
                        Changed?.Invoke();
                    }
                    try { await Task.Delay(SampleInterval, token); }
                    catch (OperationCanceledException) { break; }
                }
                IsRecording = false;
                IsFinishing = true;
                Changed?.Invoke();
                var end = MonotonicClock.Now - started;
                Elapsed = end;
                try
                {
                    SavedFile = await encoder.FinishAsync();
                    if (interrupted is not null) ErrorMessage = interrupted;
                }
                catch (Exception error) when (error is IOException or InvalidOperationException
                    or UnauthorizedAccessException or OperationCanceledException)
                {
                    if (encoder.StagingExists) RecoveryFile = encoder.StagingPath;
                    ErrorMessage = (interrupted is null ? "" : interrupted + "；")
                        + "录屏保存失败：" + error.Message
                        + (RecoveryFile is null ? "" : $"；临时文件保留在 {RecoveryFile}");
                }
                IsFinishing = false;
                lock (_gate) { _runner = null; _stopRequested = false; _cancellation?.Dispose(); _cancellation = null; }
                Changed?.Invoke();
            });
            return true;
        }
    }

    /// <summary>Concurrent stop requests share the one finalization already running.</summary>
    public Task StopAsync()
    {
        _stopRequested = true;
        lock (_gate) _cancellation?.Cancel();
        return Volatile.Read(ref _runner) ?? Task.CompletedTask;
    }

    public async ValueTask DisposeAsync()
    {
        await StopAsync();
    }

    private static string DefaultOutputDirectory()
    {
        var videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        if (string.IsNullOrEmpty(videos)) videos = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        return Path.Combine(videos, "Azki Simulator");
    }
}

/// <summary>One ffmpeg child process fed tightly packed rgb565le frames on stdin.
/// Init failure removes the staging directory; a failure after the first frame
/// keeps it, because that file may hold everything already encoded.</summary>
internal sealed class FfmpegEncoder
{
    private readonly Process _process;
    private readonly Stream _stdin;
    private readonly StringBuilder _errors = new();
    private readonly string _outputDirectory;
    private readonly string _stagingDirectory;
    private int _frames;

    public string StagingPath { get; }
    public string DestinationPath { get; }

    private FfmpegEncoder(Process process, Stream stdin, string outputDirectory, string stagingDirectory,
        string stagingPath, string destinationPath)
    {
        _process = process;
        _stdin = stdin;
        _outputDirectory = outputDirectory;
        _stagingDirectory = stagingDirectory;
        StagingPath = stagingPath;
        DestinationPath = destinationPath;
    }

    public bool StagingExists => File.Exists(StagingPath) && new FileInfo(StagingPath).Length > 0;

    public static FfmpegEncoder Launch(string ffmpegPath, int width, int height, int framerate,
        string outputDirectory, string instanceName)
    {
        if (width <= 0 || height <= 0 || width > 2_048 || height > 2_048 || width % 2 != 0 || height % 2 != 0)
            throw new IOException("画面尺寸不适合 H.264 录制");
        Directory.CreateDirectory(outputDirectory);
        var unique = Guid.NewGuid().ToString("N");
        var stagingDirectory = Path.Combine(outputDirectory, $".azki-recording-{unique}");
        var stagingPath = Path.Combine(stagingDirectory, "recording.mp4");
        Directory.CreateDirectory(stagingDirectory);
        File.SetUnixFileMode(stagingDirectory, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        var destination = Path.Combine(outputDirectory,
            $"Azki-{SafeName(instanceName)}-{DateTime.Now:yyyy-MM-dd-HH-mm-ss}-{unique}.mp4");
        // The bit-rate heuristic matches the macOS encoder: 10 bits per pixel per
        // second, clamped to the range that keeps small watchfaces from bloating.
        var bitrate = Math.Min(8_000_000, Math.Max(400_000, width * height * 10));
        var process = new Process
        {
            StartInfo = new ProcessStartInfo(ffmpegPath)
            {
                RedirectStandardInput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            },
        };
        // ArgumentList, not a command string: the staging path is interpolated straight
        // into it with no shell parsing, so no quoting rule can be abused.
        foreach (var argument in new[]
        {
            "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pixel_format", "rgb565le", "-video_size", $"{width}x{height}",
            "-framerate", framerate.ToString(), "-i", "pipe:0",
            "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
            "-b:v", bitrate.ToString(), "-g", "60", "-bf", "0", "-an",
            stagingPath,
        })
        {
            process.StartInfo.ArgumentList.Add(argument);
        }
        FfmpegEncoder encoder;
        try
        {
            process.Start();
            encoder = new FfmpegEncoder(process, process.StandardInput.BaseStream, outputDirectory,
                stagingDirectory, stagingPath, destination);
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            process.Dispose();
            TryRemoveDirectory(stagingDirectory);
            throw new IOException(error is System.ComponentModel.Win32Exception
                ? "未找到 ffmpeg；录屏需要系统安装 ffmpeg。"
                : error.Message, error);
        }
        // loglevel error keeps this small, but a full pipe would deadlock the encode,
        // so stderr is always drained and only its tail is retained.
        _ = DrainStderrAsync(process.StandardError, encoder._errors);
        return encoder;
    }

    private static async Task DrainStderrAsync(StreamReader stderr, StringBuilder errors)
    {
        try
        {
            string? line;
            while ((line = await stderr.ReadLineAsync()) is not null)
            {
                if (line.Length == 0) continue;
                lock (errors)
                {
                    errors.AppendLine(line);
                    if (errors.Length > 2_048) errors.Remove(0, errors.Length - 2_048);
                }
            }
        }
        catch (Exception) { /* stderr dying with the process is expected */ }
    }

    public Task WriteAsync(byte[] pixels, CancellationToken token)
    {
        // rgb565 is only valid when tightly packed; RawFrame.Decode enforces stride
        // == width*2, which is what the rawvideo demuxer assumes.
        _frames++;
        return _stdin.WriteAsync(pixels, token).AsTask();
    }

    public async Task<string> FinishAsync()
    {
        try
        {
            if (_frames == 0)
            {
                try { _process.Kill(entireProcessTree: true); }
                catch (InvalidOperationException) { /* already exited */ }
                catch (Exception error) when (error is IOException or PlatformNotSupportedException) { }
                TryRemoveDirectory(_stagingDirectory);
                throw new IOException("编码器尚未接收画面");
            }
            try
            {
                await _stdin.FlushAsync();
                _stdin.Close();
            }
            catch (IOException) { /* ffmpeg may have already exited */ }
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            try
            {
                await _process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                try { _process.Kill(entireProcessTree: true); }
                catch (Exception error) when (error is InvalidOperationException or IOException) { }
                await _process.WaitForExitAsync();
                throw new IOException("ffmpeg 未能在 30 秒内完成编码");
            }
            var tail = ErrorTail();
            if (_process.ExitCode != 0 || !StagingExists)
                throw new IOException(string.IsNullOrEmpty(tail) ? $"ffmpeg 以状态 {_process.ExitCode} 退出" : tail);
            File.SetUnixFileMode(StagingPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            // Publishing never replaces an existing file, mirroring the Swift
            // moveItem-to-destination behavior. The GUID suffix makes a collision
            // practically impossible; if it happens, this recording is lost rather
            // than silently overwriting an older one.
            if (File.Exists(DestinationPath)) throw new IOException("目标录屏文件已存在，未覆盖");
            File.Move(StagingPath, DestinationPath);
            TryRemoveDirectory(_stagingDirectory);
            return DestinationPath;
        }
        finally
        {
            _process.Dispose();
        }
    }

    private string ErrorTail()
    {
        lock (_errors) return _errors.ToString().Trim();
    }

    private static void TryRemoveDirectory(string path)
    {
        try { Directory.Delete(path, recursive: true); } catch (IOException) { }
    }

    /// <summary>Port of SimulatorRecordingEncoder.safeName: alphanumerics, space,
    /// dash and underscore survive; every other scalar becomes a dash; capped at
    /// 96 UTF-8 bytes; empty falls back to 设备.</summary>
    internal static string SafeName(string proposed)
    {
        var result = new StringBuilder();
        foreach (var character in proposed)
        {
            var replacement = char.IsLetterOrDigit(character) || character is ' ' or '-' or '_'
                ? character.ToString() : "-";
            if (Encoding.UTF8.GetByteCount(result.ToString() + replacement) > 96) break;
            result.Append(replacement);
        }
        var trimmed = result.ToString().Trim();
        return trimmed.Length == 0 ? "设备" : trimmed;
    }
}
