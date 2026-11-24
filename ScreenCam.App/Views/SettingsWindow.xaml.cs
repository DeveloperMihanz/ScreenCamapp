using System.Diagnostics;
using System.IO;
using System.Windows;
using ScreenCam.App.Models;
using ScreenCam.App.Services;
using ScreenCam.App.Settings;

namespace ScreenCam.App.Views;

public partial class SettingsWindow : Window
{
    private sealed record ScaleChoice(ScaleMode Mode, string Label);
    private sealed record PreviewChoice(int Width, string Label);

    private static readonly ScaleChoice[] ScaleChoices =
    {
        new(ScaleMode.Fit, "Fit — keep aspect ratio, add black bars"),
        new(ScaleMode.Fill, "Fill — keep aspect ratio, crop the edges"),
        new(ScaleMode.Stretch, "Stretch — fill the frame, may distort"),
    };

    private static readonly PreviewChoice[] PreviewChoices =
    {
        new(320, "Low (320 px wide)"),
        new(640, "Normal (640 px wide)"),
        new(960, "High (960 px wide)"),
    };

    private readonly AppSettings _settings;

    /// <summary>The edited copy; valid when the dialog returns true.</summary>
    public AppSettings Result => _settings;

    public SettingsWindow(AppSettings current, string diagnostics)
    {
        InitializeComponent();
        _settings = current.Clone();

        ScaleModeBox.ItemsSource = ScaleChoices;
        ScaleModeBox.SelectedValue = _settings.ScaleMode;
        PreviewSizeBox.ItemsSource = PreviewChoices;
        PreviewSizeBox.SelectedValue = PreviewChoices.Any(p => p.Width == _settings.PreviewMaxWidth) ? _settings.PreviewMaxWidth : 640;

        ShowCursorBox.IsChecked = _settings.ShowCursor;
        HideBorderBox.IsChecked = _settings.HideCaptureBorder;
        PreviewBox.IsChecked = _settings.PreviewEnabled;
        AutoStartBox.IsChecked = _settings.AutoStartCamera;
        DiagnosticsText.Text = diagnostics;
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        _settings.ShowCursor = ShowCursorBox.IsChecked == true;
        _settings.HideCaptureBorder = HideBorderBox.IsChecked == true;
        _settings.PreviewEnabled = PreviewBox.IsChecked == true;
        _settings.AutoStartCamera = AutoStartBox.IsChecked == true;
        if (ScaleModeBox.SelectedValue is ScaleMode mode)
            _settings.ScaleMode = mode;
        if (PreviewSizeBox.SelectedValue is int width)
            _settings.PreviewMaxWidth = width;
        DialogResult = true;
    }

    private static void OpenFolder(string path)
    {
        try
        {
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            Logger.Error($"Could not open {path}", ex);
        }
    }

    private void OpenSettingsFolder_Click(object sender, RoutedEventArgs e) => OpenFolder(SettingsStore.Directory);

    private void OpenLogFolder_Click(object sender, RoutedEventArgs e) => OpenFolder(Logger.Directory);
}
