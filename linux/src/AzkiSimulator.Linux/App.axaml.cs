using System.Diagnostics;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.ViewModels;
using AzkiSimulator.Linux.Views;

namespace AzkiSimulator.Linux;

public class App : Application
{
    private readonly Dictionary<string, DeviceWindow> _deviceWindows = new();

    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        // Command bodies reach subprocesses and sockets; a fault that escapes one must be
        // written down rather than end the process with no record of what failed.
        var log = DataLocations.GuiLogPath;
        UiFault.Handler = error => AppendFault(log, error);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            AppendFault(log, args.Exception);
            args.SetObserved();
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception error) AppendFault(log, error);
        };

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var manager = new ManagerViewModel();
            manager.OpenDeviceRequested += instance =>
            {
                // One window per instance: a second window would start a second sandboxed
                // session writing the same instance storage.
                if (_deviceWindows.TryGetValue(instance.Id, out var existing))
                {
                    existing.Activate();
                    return Task.CompletedTask;
                }
                if (manager.Layout is not { } layout)
                {
                    manager.StatusText = "未找到 Azki Simulator 引擎目录，无法打开设备。";
                    return Task.CompletedTask;
                }
                var window = new DeviceWindow { DataContext = new DeviceViewModel(layout, instance) };
                window.Closed += (_, _) => _deviceWindows.Remove(instance.Id);
                _deviceWindows[instance.Id] = window;
                if (desktop.MainWindow is { } owner) window.Show(owner);
                else window.Show();
                return Task.CompletedTask;
            };
            desktop.MainWindow = new ManagerWindow { DataContext = manager };
            if (!UnixNative.SupportedAbi)
                manager.StatusText = $"当前主机架构 {RuntimeInformation.OSArchitecture} 的文件属性布局未验证，会话功能已停用。";
        }
        base.OnFrameworkInitializationCompleted();
    }

    private static readonly object LogGate = new();

    private static void AppendFault(string path, Exception? error)
    {
        if (error is null) return;
        Trace.WriteLine($"[azki-gui] {error}");
        try
        {
            lock (LogGate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.AppendAllText(path, $"{DateTime.Now:O} {error.GetType().Name}: {error}\n");
            }
        }
        catch (IOException) { /* a log that cannot be written must not fault again */ }
    }
}
