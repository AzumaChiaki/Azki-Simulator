using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using AzkiSimulator.Linux.Core;
using AzkiSimulator.Linux.ViewModels;

namespace AzkiSimulator.Linux.Views;

public partial class ManagerWindow : Window
{
    private ManagerViewModel? _attached;

    public ManagerWindow()
    {
        InitializeComponent();
        // The profile picker follows the firmware list and the chosen model, nothing else:
        // subscribing to every property change would rebuild the list (and drop the current
        // selection) each time the status line updated.
        DataContextChanged += (_, _) => Attach();
        NewModel.SelectionChanged += (_, _) => SyncProfileOptions();
        Attach();
    }

    private ManagerViewModel? Model => DataContext as ManagerViewModel;

    private void Attach()
    {
        if (_attached is { } previous) previous.FirmwareItems.CollectionChanged -= OnFirmwareChanged;
        _attached = Model;
        if (_attached is { } model) model.FirmwareItems.CollectionChanged += OnFirmwareChanged;
        SyncProfileOptions();
    }

    private void OnFirmwareChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
        => SyncProfileOptions();

    private void SyncProfileOptions()
    {
        if (Model is not { } model) return;
        var selected = (NewModel.SelectedItem as FirmwarePreset)?.Model;
        var previous = NewProfile.SelectedItem as string;
        NewProfile.ItemsSource = model.FirmwareItems
            .Where(item => selected is null || item.Model == selected)
            .Select(item => item.Id)
            .ToList();
        if (previous is not null && NewProfile.Items.Cast<string?>().Contains(previous)) NewProfile.SelectedItem = previous;
    }

    private async void CreateInstance_Click(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not ManagerViewModel model) return;
        var preset = NewModel.SelectedItem as FirmwarePreset;
        try
        {
            var created = await model.CreateInstanceAsync(
                NewName.Text ?? "", preset?.Model ?? "", NewProfile.SelectedItem as string,
                NewSerial.Text, NewAddress.Text, NewSkipOobe.IsChecked ?? true);
            if (created is not null)
            {
                NewName.Text = "";
                NewSerial.Text = "";
                NewAddress.Text = "";
            }
        }
        catch (Exception error)
        {
            UiFault.Report(error);
            model.StatusText = $"创建实例失败：{error.Message}";
        }
    }

    private void OpenDevice_DoubleTapped(object? sender, TappedEventArgs e)
    {
        if (DataContext is ManagerViewModel model && model.SelectedInstance is not null)
            model.OpenDeviceCommand.Execute(null);
    }

    private async void DeleteAll_Click(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not ManagerViewModel model) return;
        try
        {
            var count = model.FirmwareItems.Count;
            // Destructive and irreversible, so it confirms first and defaults to no
            // button — the same posture as the macOS settings sheet.
            var confirm = new FluentAvalonia.UI.Controls.ContentDialog
            {
                Title = "删除全部镜像",
                Content = $"将清空整个固件库：移除全部 {count} 个已导入的 OTA 及其对象文件，并回收对应磁盘空间。此操作不可撤销，已创建的实例记录保留。",
                PrimaryButtonText = $"删除全部 {count} 个镜像",
                CloseButtonText = "取消",
                DefaultButton = FluentAvalonia.UI.Controls.ContentDialogButton.None,
            };
            if (await confirm.ShowAsync() == FluentAvalonia.UI.Controls.ContentDialogResult.Primary)
                await model.DeleteAllFirmwareAsync();
        }
        catch (Exception error) { UiFault.Report(error); }
    }

    private async void ImportFirmware_Click(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not ManagerViewModel model) return;
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = "选择完整的 OTA 固件包",
                AllowMultiple = false,
                FileTypeFilter = new[] { new FilePickerFileType("OTA 包") { Patterns = new[] { "*.zip", "*.bin", "*.ota" } },
                                         new FilePickerFileType("所有文件") { Patterns = new[] { "*" } } },
            });
            var file = files.Count > 0 ? files[0] : null;
            var preset = NewModel.SelectedItem as FirmwarePreset;
            await model.ImportFirmwareAsync(file, preset?.Model);
        }
        catch (Exception error)
        {
            UiFault.Report(error);
            model.StatusText = $"选择固件包失败：{error.Message}";
        }
    }
}
