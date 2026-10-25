using System.Collections.Specialized;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.Services;
using AzkiSimulator.Linux.ViewModels;

namespace AzkiSimulator.Linux.Views;

public partial class DeviceWindow : Window
{
    private DeviceViewModel? _attached;
    private bool _pressing;
    private (int X, int Y) _lastPoint;
    private bool _scrollQueued;

    public DeviceWindow()
    {
        InitializeComponent();
        Canvas.PointerPressed += OnPointerPressed;
        Canvas.PointerMoved += OnPointerMoved;
        Canvas.PointerReleased += OnPointerReleased;
        // Releasing at the pressed position: sending (0,0) would lift the finger in the
        // corner and read to the guest as a drag to the edge.
        Canvas.PointerCaptureLost += (_, _) => ReleasePress();
        DataContextChanged += (_, _) => Attach();
        Closed += (_, _) => Detach();
    }

    private DeviceViewModel? Model => DataContext as DeviceViewModel;

    private void Attach()
    {
        if (_attached is { } previous)
        {
            previous.LogLines.CollectionChanged -= OnLogLinesChanged;
            foreach (var channel in previous.AdcChannels) channel.PropertyChanged -= OnChannelTextChanged;
        }
        _attached = Model;
        if (_attached is not { } model) return;
        model.LogLines.CollectionChanged += OnLogLinesChanged;
        foreach (var channel in model.AdcChannels) channel.PropertyChanged += OnChannelTextChanged;
    }

    private void Detach()
    {
        var model = _attached;
        if (model is null) return;
        model.LogLines.CollectionChanged -= OnLogLinesChanged;
        foreach (var channel in model.AdcChannels) channel.PropertyChanged -= OnChannelTextChanged;
        _attached = null;
        // Closing the window owns the session teardown; it is awaited by the dispatcher
        // so a fault here cannot escape through an async void handler.
        Dispatcher.UIThread.Post(async () =>
        {
            try { await model.DisposeAsync(); } catch (Exception error) { UiFault.Report(error); }
        });
    }

    private void OnLogLinesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.Action != NotifyCollectionChangedAction.Add || _scrollQueued) return;
        _scrollQueued = true;
        // One scroll per batch rather than per line: a burst of log output would otherwise
        // lay out the list once per appended row.
        Dispatcher.UIThread.Post(() =>
        {
            _scrollQueued = false;
            if (LogList.Items.Count > 0 && LogList.Items[^1] is { } last) LogList.ScrollIntoView(last);
        });
    }

    private void OnChannelTextChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (sender is PmuAdcChannelViewModel row && e.PropertyName == nameof(row.Text))
            Model?.UpdateAdcDraft(row.Channel, row.Text);
    }

    private void OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        _pressing = true;
        e.Pointer.Capture(Canvas);
        if (TryPoint(e, out var x, out var y)) Send(x, y, true);
    }

    private void OnPointerMoved(object? sender, PointerEventArgs e)
    {
        if (!_pressing || !TryPoint(e, out var x, out var y)) return;
        Send(x, y, true);
    }

    private void OnPointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        e.Pointer.Capture(null);
        if (_pressing && TryPoint(e, out var x, out var y)) Send(x, y, false);
        _pressing = false;
    }

    private void ReleasePress()
    {
        if (!_pressing) return;
        _pressing = false;
        var (x, y) = _lastPoint;
        Send(x, y, false);
    }

    private bool TryPoint(PointerEventArgs e, out int x, out int y)
    {
        x = y = 0;
        if (Model is not { } model) return false;
        var point = e.GetPosition(Canvas);
        var input = CanvasGeometry.Input(point, new global::Avalonia.Rect(0, 0, Canvas.Bounds.Width, Canvas.Bounds.Height),
            new global::Avalonia.Size(model.PixelWidth, model.PixelHeight));
        if (input is null) return false;
        (x, y) = input.Value;
        _lastPoint = (x, y);
        return true;
    }

    private void Send(int x, int y, bool down)
    {
        try { Model?.Touch(x, y, down); }
        catch (Exception error) { UiFault.Report(error); }
    }

    private async void Install_Click(object? sender, RoutedEventArgs e)
    {
        if (Model is not { } model) return;
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = "选择快应用或表盘包",
                AllowMultiple = false,
                FileTypeFilter = new[] { new FilePickerFileType("安装包") { Patterns = new[] { "*.zip", "*.rpk", "*.bin" } },
                                         new FilePickerFileType("所有文件") { Patterns = new[] { "*" } } },
            });
            if (files.Count == 0) return;
            await model.InstallAsync(files[0].Path.LocalPath, "package");
        }
        catch (Exception error)
        {
            UiFault.Report(error);
            model.StatusText = $"选择安装包失败：{error.Message}";
        }
    }
}
