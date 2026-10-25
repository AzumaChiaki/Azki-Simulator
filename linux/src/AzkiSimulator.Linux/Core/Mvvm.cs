using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace AzkiSimulator.Linux.Core;

/// <summary>Last line of reporting for faults that escape a command. Without it an
/// async void command handler dies with the process and leaves no trace.</summary>
public static class UiFault
{
    public static Action<Exception>? Handler;

    public static void Report(Exception error)
    {
        var handler = Handler;
        if (handler is null) return;
        try { handler(error); } catch (Exception) { /* reporting must never fault a second time */ }
    }
}

public abstract class ObservableObject : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void Raise([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new(name));

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        Raise(name);
        return true;
    }
}

/// <summary>Async command that refuses to overlap itself and reports anything it throws.
/// Command bodies reach processes and sockets, so a second click during an in-flight
/// start would otherwise spawn a second sandboxed session for the same instance.</summary>
public sealed class AsyncRelayCommand(Func<Task> execute, Func<bool>? canExecute = null) : ICommand
{
    private bool _running;
    public event EventHandler? CanExecuteChanged;
    public bool CanExecute(object? parameter) => !_running && (canExecute?.Invoke() ?? true);
    public async void Execute(object? parameter)
    {
        if (_running) return;
        _running = true;
        CanExecuteChanged?.Invoke(this, EventArgs.Empty);
        try
        {
            await execute();
        }
        catch (Exception error)
        {
            UiFault.Report(error);
        }
        finally
        {
            _running = false;
            CanExecuteChanged?.Invoke(this, EventArgs.Empty);
        }
    }
    public void RaiseCanExecute() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}
