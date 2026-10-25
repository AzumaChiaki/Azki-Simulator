using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using AzkiSimulator.Linux.Core;

namespace AzkiSimulator.Linux.Services;

/// <summary>
/// Polls frame.raw on a background loop at the macOS display cadence
/// (≈60 fps, one read in flight, missed deadlines skipped) and blits RGB565
/// rows straight into a WriteableBitmap — the frame format already matches.
/// </summary>
public sealed class DeviceFrameService : IAsyncDisposable
{
    private static readonly TimeSpan Interval = TimeSpan.FromMilliseconds(16.666_667);
    private static readonly TimeSpan ReportAfter = TimeSpan.FromSeconds(1);
    private readonly FrameReader _reader;
    private readonly CancellationTokenSource _cancellation = new();
    private readonly object _gate = new();
    private Task? _loop;
    private WriteableBitmap? _bitmap;
    private RawFrame? _pending;
    private bool _blitQueued;

    public DeviceFrameService(string sessionDirectory, SessionManifest manifest)
    {
        _reader = new FrameReader(sessionDirectory, manifest);
    }

    public WriteableBitmap? Current => _bitmap;

    /// <summary>Every event is raised on the UI thread: subscribers bind to views.</summary>
    public event Action<WriteableBitmap>? Updated;
    public event Action<Exception>? Failed;
    public event Action<Exception>? Disconnected;

    public void Start() => _loop ??= Task.Run(RunAsync);

    private async Task RunAsync()
    {
        var token = _cancellation.Token;
        TimeSpan? failingSince = null;
        while (!token.IsCancellationRequested)
        {
            RawFrame? frame = null;
            try
            {
                frame = _reader.ReadNext();
                failingSince = null;
            }
            catch (Exception error) when (error is BridgeConnectionClosed)
            {
                Dispatch(() => Disconnected?.Invoke(error));
                return;
            }
            catch (Exception error) when (error is IOException or InvalidDataException)
            {
                // A torn or short frame is retried at the next deadline; only a failure
                // that keeps happening for a second is worth telling the user about.
                var now = MonotonicClock.Now;
                failingSince ??= now;
                if (now - failingSince.Value >= ReportAfter) Dispatch(() => Failed?.Invoke(error));
            }
            if (frame is not null) QueueBlit(frame);
            try { await Task.Delay(Interval, token); }
            catch (OperationCanceledException) { return; }
        }
    }

    /// <summary>Keeps at most one blit queued: if the UI thread stalls, frames are
    /// dropped rather than accumulating a backlog that replays stale screens.</summary>
    private void QueueBlit(RawFrame frame)
    {
        lock (_gate)
        {
            _pending = frame;
            if (_blitQueued) return;
            _blitQueued = true;
        }
        Dispatcher.UIThread.Post(RunBlit);
    }

    private void RunBlit()
    {
        RawFrame? frame;
        lock (_gate)
        {
            _blitQueued = false;
            frame = _pending;
            _pending = null;
        }
        if (frame is not null) Blit(frame);
    }

    private static void Dispatch(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess()) action();
        else Dispatcher.UIThread.Post(action);
    }

    private void Blit(RawFrame frame)
    {
        if (_bitmap is null || _bitmap.PixelSize.Width != frame.Width || _bitmap.PixelSize.Height != frame.Height)
        {
            _bitmap?.Dispose();
            _bitmap = new WriteableBitmap(new PixelSize(frame.Width, frame.Height),
                new Vector(96, 96), PixelFormat.Rgb565, AlphaFormat.Opaque);
        }
        using var buffer = _bitmap.Lock();
        // Rows are already little-endian RGB565 at stride = width * 2.
        var rowBytes = frame.Width * 2;
        for (var row = 0; row < frame.Height; row++)
            Marshal.Copy(frame.Pixels, row * frame.Stride,
                buffer.Address + (nint)(row * buffer.RowBytes), rowBytes);
        Updated?.Invoke(_bitmap);
    }

    public async ValueTask DisposeAsync()
    {
        _cancellation.Cancel();
        if (_loop is not null)
        {
            try { await _loop; } catch (Exception) { }
        }
        _reader.Dispose();
        // The bitmap belongs to the UI thread that renders it, even when the window
        // closed on a different dispatcher turn.
        Dispatch(() => { _bitmap?.Dispose(); _bitmap = null; });
    }
}

/// <summary>Port of SimulatorCanvasGeometry content/input mapping.</summary>
public static class CanvasGeometry
{
    public static Rect ContentRect(Rect bounds, Size pixelSize)
    {
        if (!double.IsFinite(bounds.Width) || !double.IsFinite(bounds.Height) || bounds.Width <= 0 || bounds.Height <= 0 ||
            !double.IsFinite(pixelSize.Width) || !double.IsFinite(pixelSize.Height) || pixelSize.Width <= 0 || pixelSize.Height <= 0)
            return default;
        var scale = Math.Min(bounds.Width / pixelSize.Width, bounds.Height / pixelSize.Height);
        var size = new Size(pixelSize.Width * scale, pixelSize.Height * scale);
        return new Rect(bounds.X + (bounds.Width - size.Width) / 2,
            bounds.Y + (bounds.Height - size.Height) / 2, size.Width, size.Height);
    }

    public static (int X, int Y)? Input(Point point, Rect bounds, Size pixelSize)
    {
        var rect = ContentRect(bounds, pixelSize);
        if (!double.IsFinite(point.X) || !double.IsFinite(point.Y) || rect.Width <= 0 || rect.Height <= 0 ||
            point.X < rect.X || point.X >= rect.Right || point.Y < rect.Y || point.Y >= rect.Bottom)
            return null;
        var x = Math.Min((int)pixelSize.Width - 1, (int)((point.X - rect.X) / rect.Width * pixelSize.Width));
        var y = Math.Min((int)pixelSize.Height - 1, (int)((point.Y - rect.Y) / rect.Height * pixelSize.Height));
        return (x, y);
    }
}
