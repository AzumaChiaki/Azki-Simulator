using Avalonia.Controls;
using Avalonia.Interactivity;
using AzkiSimulator.Linux.ViewModels;

namespace AzkiSimulator.Linux.Views;

public partial class FirmwareSettingsWindow : Window
{
    public FirmwareSettingsWindow()
    {
        InitializeComponent();
    }

    private void Cancel_Click(object? sender, RoutedEventArgs e) => Close();
}
