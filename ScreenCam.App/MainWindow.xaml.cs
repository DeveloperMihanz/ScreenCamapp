using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;
using ScreenCam.App.Models;
using ScreenCam.App.Services;
using ScreenCam.App.Settings;
using ScreenCam.App.Views;

namespace ScreenCam.App;

public partial class MainWindow : Window
{
    private readonly CaptureEngine _engine = new();
    private AppSettings _settings;
    private IReadOnlyList<MonitorInfo> _monitors = Array.Empty<MonitorInfo>();

    private readonly DispatcherTimer _previewTimer;
    private readonly DispatcherTimer _statusTimer;
    private readonly DispatcherTimer _displayChangeTimer;

    private WriteableBitmap? _previewBitmap;
    private CameraState _cameraState = CameraState.Stopped;
    private bool _engineReady;
    private bool _suppressOptionEvents;
    private bool _selecting;
    private Action? _bannerAction;

    public MainWindow()
    {
        InitializeComponent();
        _settings = SettingsStore.Load();

        _suppressOptionEvents = true;
        ResolutionBox.ItemsSource = ResolutionOption.All;
        ResolutionBox.SelectedItem = ResolutionOption.All.First(r => r.Width == _settings.OutputWidth && r.Height == _settings.OutputHeight);
        FrameRateBox.ItemsSource = FrameRateOption.All;
        FrameRateBox.SelectedItem = FrameRateOption.All.First(f => f.Fps == _settings.FrameRate);
        _suppressOptionEvents = false;

        var version = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = version is null ? string.Empty : $"v{version.Major}.{version.Minor}.{version.Build}";
        RestoreWindowPlacement();

        _previewTimer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(66) };
        _previewTimer.Tick += (_, _) => UpdatePreview();
        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _statusTimer.Tick += (_, _) => UpdateStatus();
        _displayChangeTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
        _displayChangeTimer.Tick += (_, _) =>
        {
            _displayChangeTimer.Stop();
            OnDisplayConfigurationChanged();
        };

        Loaded += OnLoaded;
        Closing += OnClosing;
        SystemEvents.DisplaySettingsChanged += OnSystemDisplaySettingsChanged;
    }

    // ------------------------------------------------------------------
    // Startup / shutdown
    // ------------------------------------------------------------------
    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        var init = _engine.Initialize();
        if (!init.Success)
        {
            _engineReady = false;
            ShowBanner(init.Message, isError: true);
            SetCameraState(CameraState.Error, init.Message);
            SelectAreaButton.IsEnabled = false;
            ResolutionBox.IsEnabled = false;
            FrameRateBox.IsEnabled = false;
            StartButton.IsEnabled = false;
            return;
        }
        _engineReady = true;

        _monitors = MonitorService.GetMonitors();
        UpdateRegionText();
        RefreshCapabilityBanner();
        SetCameraState(_engine.IsVirtualCameraSupported ? CameraState.Stopped : CameraState.Unsupported);
        EnsureCaptureState();

        _previewTimer.Start();
        _statusTimer.Start();

        if (_settings.AutoStartCamera && _engine.IsVirtualCameraSupported && _settings.Region is not null)
        {
            Logger.Info("Auto-starting the virtual camera");
            _ = StartCameraAsync(interactive: false);
        }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        _previewTimer.Stop();
        _statusTimer.Stop();
        _displayChangeTimer.Stop();
        SystemEvents.DisplaySettingsChanged -= OnSystemDisplaySettingsChanged;

        SaveWindowPlacement();
        SettingsStore.Save(_settings);

        if (_engineReady)
        {
            _engine.StopCamera();   // removes the camera from Windows
            _engine.StopCapture();
        }
        _engine.Dispose();
    }

    // ------------------------------------------------------------------
    // Capture management
    // ------------------------------------------------------------------
    private PixelRect? ResolveRegion(out string? problem)
    {
        problem = null;
        var region = _settings.Region;
        if (region is null)
            return null;
        var monitor = MonitorService.FindForRegion(region, _monitors);
        if (monitor is null)
        {
            problem = "The display used for the saved screen area is not connected. Select a new screen area.";
            return null;
        }
        return region.ResolveOn(monitor);
    }

    /// <summary>
    /// Runs exactly one capture session when it is needed (preview enabled or
    /// camera running) and stops it otherwise, so no CPU is wasted.
    /// </summary>
    private bool EnsureCaptureState(bool forCamera = false)
    {
        if (!_engineReady)
            return false;

        bool needed = _settings.Region is not null && (_settings.PreviewEnabled || forCamera || _engine.IsCameraActive);
        if (!needed)
        {
            if (_engine.IsCaptureRunning)
                _engine.StopCapture();
            UpdatePreviewPlaceholder();
            return true;
        }

        var rect = ResolveRegion(out var problem);
        if (rect is null)
        {
            if (_engine.IsCaptureRunning)
                _engine.StopCapture();
            if (problem is not null)
                ShowBanner(problem, isError: false, "Select area", () => _ = SelectAreaAsync());
            UpdatePreviewPlaceholder();
            return false;
        }

        var result = _engine.ApplyCapture(rect.Value, _settings);
        if (!result.Success)
        {
            ShowBanner($"Screen capture failed: {result.Message}", isError: true);
            UpdatePreviewPlaceholder();
            return false;
        }
        UpdatePreviewPlaceholder();
        return true;
    }

    private void OnSystemDisplaySettingsChanged(object? sender, EventArgs e)
    {
        // Debounce: Windows raises several notifications while a mode change settles.
        Dispatcher.BeginInvoke(() =>
        {
            _displayChangeTimer.Stop();
            _displayChangeTimer.Start();
        });
    }

    private void OnDisplayConfigurationChanged()
    {
        if (!_engineReady)
            return;
        Logger.Info("Display configuration changed; re-applying the screen area");
        _monitors = MonitorService.GetMonitors();
        UpdateRegionText();
        if (EnsureCaptureState())
            RefreshCapabilityBanner();
    }

    // ------------------------------------------------------------------
    // Screen area selection
    // ------------------------------------------------------------------
    private async void SelectAreaButton_Click(object sender, RoutedEventArgs e) => await SelectAreaAsync();

    private async Task<bool> SelectAreaAsync()
    {
        if (_selecting || !_engineReady)
            return false;
        _selecting = true;
        SelectAreaButton.IsEnabled = false;
        var previousState = WindowState;
        try
        {
            // Get ScreenCam out of the way so the area behind it can be selected.
            Hide();
            await Task.Delay(200);
            double aspect = (double)_settings.OutputWidth / _settings.OutputHeight;
            var region = await RegionSelector.SelectAsync(_settings.Region, aspect);
            if (region is null)
                return false;

            _settings.Region = region;
            SettingsStore.Save(_settings);
            Logger.Info($"Screen area selected: {region} on {region.MonitorDeviceName}");
            _monitors = MonitorService.GetMonitors();
            UpdateRegionText();
            if (EnsureCaptureState())
                RefreshCapabilityBanner();
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error("Area selection failed", ex);
            ShowBanner($"Area selection failed: {ex.Message}", isError: true);
            return false;
        }
        finally
        {
            Show();
            WindowState = previousState == WindowState.Minimized ? WindowState.Normal : previousState;
            Activate();
            SelectAreaButton.IsEnabled = true;
            _selecting = false;
        }
    }

    private void UpdateRegionText()
    {
        var region = _settings.Region;
        if (region is null)
        {
            RegionText.Text = "No area selected";
            RegionMonitorText.Text = "Choose the part of your screen the camera should show.";
            return;
        }
        var monitor = MonitorService.FindForRegion(region, _monitors);
        if (monitor is null)
        {
            RegionText.Text = $"{region.Width} × {region.Height} px";
            RegionMonitorText.Text = "Display not connected";
            return;
        }
        var rect = region.ResolveOn(monitor);
        RegionText.Text = $"{rect.Width} × {rect.Height} px";
        RegionMonitorText.Text = $"{monitor.DisplayName}\nPosition {rect.Left - monitor.Bounds.Left}, {rect.Top - monitor.Bounds.Top}";
    }

    // ------------------------------------------------------------------
    // Output options
    // ------------------------------------------------------------------
    private void OutputOption_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressOptionEvents)
            return;
        if (ResolutionBox.SelectedItem is ResolutionOption res)
        {
            _settings.OutputWidth = res.Width;
            _settings.OutputHeight = res.Height;
        }
        if (FrameRateBox.SelectedItem is FrameRateOption fps)
            _settings.FrameRate = fps.Fps;
        SettingsStore.Save(_settings);
        UpdatePreviewInfo();
        if (_engine.IsCaptureRunning)
            EnsureCaptureState();
    }

    // ------------------------------------------------------------------
    // Virtual camera
    // ------------------------------------------------------------------
    private async void StartButton_Click(object sender, RoutedEventArgs e) => await StartCameraAsync(interactive: true);

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        _engine.StopCamera();
        SetCameraState(CameraState.Stopped);
        EnsureCaptureState();
    }

    private async Task StartCameraAsync(bool interactive)
    {
        if (!_engineReady || _cameraState is CameraState.Active or CameraState.Starting)
            return;

        if (!_engine.IsVirtualCameraSupported)
        {
            SetCameraState(CameraState.Unsupported);
            if (interactive)
            {
                MessageBox.Show(this,
                    $"The Windows Virtual Camera API is not available on this PC (Windows build {_engine.WindowsBuild}).\n\n" +
                    "ScreenCam Virtual Camera requires Windows 11 (build 22000 or later).",
                    "ScreenCam", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            return;
        }

        if (_settings.Region is null)
        {
            if (!interactive || !await SelectAreaAsync())
                return;
        }

        SetCameraState(CameraState.Starting);
        await System.Windows.Threading.Dispatcher.Yield(DispatcherPriority.Background); // let the "Starting…" state render

        if (!EnsureCaptureState(forCamera: true))
        {
            SetCameraState(CameraState.Error, "The screen area could not be captured. See the message above.");
            return;
        }

        var result = _engine.StartCamera();
        if (!result.Success)
        {
            SetCameraState(CameraState.Error, result.Message);
            if (result.Code == NativeMethods.SC_E_SOURCE_NOT_REGISTERED)
                RefreshCapabilityBanner();
            EnsureCaptureState();
            return;
        }
        SetCameraState(CameraState.Active);
    }

    private void SetCameraState(CameraState state, string? detail = null)
    {
        _cameraState = state;
        var red = (Brush)FindResource("DangerBrush");
        var green = (Brush)FindResource("SuccessBrush");

        switch (state)
        {
            case CameraState.Unsupported:
                StatusDot.Fill = (Brush)FindResource("NeutralBrush");
                StatusText.Text = "Not available";
                StatusDetail.Text = detail ?? $"Requires Windows 11 (build 22000+). This PC runs build {_engine.WindowsBuild}. The preview still works.";
                StartButton.Content = "START VIRTUAL CAMERA";
                StartButton.Background = red;
                StartButton.IsEnabled = false;
                StopButton.IsEnabled = false;
                break;
            case CameraState.Stopped:
                StatusDot.Fill = red;
                StatusText.Text = "Stopped";
                StatusDetail.Text = detail ?? "Start the camera to make “ScreenCam Virtual Camera” available to other apps.";
                StartButton.Content = "START VIRTUAL CAMERA";
                StartButton.Background = red;
                StartButton.IsEnabled = _engineReady;
                StopButton.IsEnabled = false;
                break;
            case CameraState.Starting:
                StatusDot.Fill = (Brush)FindResource("WarningBrush");
                StatusText.Text = "Starting…";
                StatusDetail.Text = detail ?? "Registering the virtual camera with Windows.";
                StartButton.IsEnabled = false;
                StopButton.IsEnabled = false;
                break;
            case CameraState.Active:
                StatusDot.Fill = green;
                StatusText.Text = "Active";
                StatusDetail.Text = detail ?? "Ready — choose “ScreenCam Virtual Camera” in Zoom, Teams, Meet, OBS or any camera app.";
                StartButton.Content = "VIRTUAL CAMERA ACTIVE";
                StartButton.Background = green;
                StartButton.IsEnabled = true; // stays green; clicking again does nothing
                StopButton.IsEnabled = true;
                break;
            case CameraState.Error:
                StatusDot.Fill = red;
                StatusText.Text = "Error";
                StatusDetail.Text = detail ?? "The virtual camera could not be started.";
                StartButton.Content = "START VIRTUAL CAMERA";
                StartButton.Background = red;
                StartButton.IsEnabled = _engineReady;
                StopButton.IsEnabled = _engine.IsCameraActive;
                break;
        }
    }

    // ------------------------------------------------------------------
    // Periodic UI updates
    // ------------------------------------------------------------------
    private void UpdatePreview()
    {
        if (!_engineReady || !_settings.PreviewEnabled || WindowState == WindowState.Minimized || !IsVisible)
            return;
        if (_engine.UpdatePreview(ref _previewBitmap) && !ReferenceEquals(PreviewImage.Source, _previewBitmap))
            PreviewImage.Source = _previewBitmap;
        UpdatePreviewPlaceholder();
    }

    private void UpdatePreviewPlaceholder()
    {
        UpdatePreviewInfo();
        string? message = null;
        if (!_engineReady)
            message = "The native capture engine is not available.";
        else if (_settings.Region is null)
            message = "Select a screen area to start the preview";
        else if (!_settings.PreviewEnabled)
            message = "Preview is turned off in Settings" + (_engine.IsCameraActive ? "\n(the camera is still broadcasting)" : string.Empty);
        else if (!_engine.IsCaptureRunning)
            message = "Capture is not running";
        else if (PreviewImage.Source is null)
            message = "Waiting for the first frame…";

        PreviewPlaceholder.Visibility = message is null ? Visibility.Collapsed : Visibility.Visible;
        PreviewImage.Visibility = message is null ? Visibility.Visible : Visibility.Hidden;
        if (message is not null)
            PreviewPlaceholderText.Text = message;
        if (!_engine.IsCaptureRunning)
        {
            PreviewImage.Source = null;
            _previewBitmap = null;
        }
    }

    private void UpdatePreviewInfo() =>
        PreviewInfoText.Text = $"Camera output {_settings.OutputWidth} × {_settings.OutputHeight} · {_settings.FrameRate} fps";

    private void UpdateStatus()
    {
        var status = _engine.GetStatus();
        if (status is null)
            return;

        if (_cameraState == CameraState.Active)
        {
            if (!status.CameraActive)
            {
                SetCameraState(CameraState.Error, "The virtual camera stopped unexpectedly. Click Start to try again.");
            }
            else if (status.ConsumerStreaming)
            {
                string fmt = string.IsNullOrEmpty(status.ConsumerFormat) ? string.Empty : $" ({status.ConsumerFormat})";
                StatusDetail.Text = $"Streaming to an app · {status.ConsumerWidth} × {status.ConsumerHeight} @ {status.ConsumerFps:0} fps{fmt}";
            }
            else
            {
                StatusDetail.Text = "Ready — choose “ScreenCam Virtual Camera” in Zoom, Teams, Meet, OBS or any camera app.";
            }
        }

        if (status.CaptureLost && _settings.Region is not null && !_displayChangeTimer.IsEnabled)
            _displayChangeTimer.Start(); // e.g. monitor unplugged: re-evaluate

        FooterText.Text = status.CaptureRunning
            ? $"Capture {status.CaptureFps:0.0} fps (changes only) · {(status.GpuProcessing ? "GPU" : "CPU")} scaling · {status.FramesPublished:N0} frames sent to the camera"
              + (status.ConsumerFrames > 0 ? $" · {status.ConsumerFrames:N0} frames delivered to apps" : string.Empty)
            : "Capture stopped";
    }

    // ------------------------------------------------------------------
    // Banner / capabilities
    // ------------------------------------------------------------------
    private void RefreshCapabilityBanner()
    {
        if (!_engineReady)
            return;
        if (!_engine.IsCaptureSupported)
            ShowBanner("Windows Graphics Capture is not available on this system. Windows 10 version 2004 or later is required.", isError: true);
        else if (!_engine.IsVirtualCameraSupported)
            ShowBanner($"This PC runs Windows build {_engine.WindowsBuild}. The Windows Virtual Camera API requires Windows 11 (build 22000 or later), " +
                       "so the camera cannot be started. The preview still works.", isError: false);
        else if (!_engine.IsSourceRegistered)
            ShowBanner("The ScreenCam camera component is not registered. Run the installer (or Scripts\\install.ps1) as Administrator, then click “Check again”.",
                       isError: false, "Check again", RefreshCapabilityBanner);
        else
            HideBanner();
    }

    private void ShowBanner(string text, bool isError, string? actionText = null, Action? action = null)
    {
        BannerText.Text = text;
        Banner.Background = (Brush)FindResource(isError ? "BannerErrorBrush" : "BannerWarningBrush");
        Banner.BorderBrush = (Brush)FindResource(isError ? "BannerErrorBorderBrush" : "BannerWarningBorderBrush");
        _bannerAction = action;
        BannerButton.Content = actionText;
        BannerButton.Visibility = action is null ? Visibility.Collapsed : Visibility.Visible;
        Banner.Visibility = Visibility.Visible;
    }

    private void HideBanner()
    {
        Banner.Visibility = Visibility.Collapsed;
        _bannerAction = null;
    }

    private void BannerButton_Click(object sender, RoutedEventArgs e) => _bannerAction?.Invoke();

    // ------------------------------------------------------------------
    // Settings
    // ------------------------------------------------------------------
    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        string yes(bool b) => b ? "yes" : "no";
        string diagnostics = _engineReady
            ? $"Windows build: {_engine.WindowsBuild}\n" +
              $"Windows Virtual Camera API: {yes(_engine.IsVirtualCameraSupported)}\n" +
              $"Camera component registered: {yes(_engine.IsSourceRegistered)}\n" +
              $"Windows Graphics Capture: {yes(_engine.IsCaptureSupported)}\n" +
              $"Settings file: {SettingsStore.FilePath}\nLog file: {Logger.FilePath}"
            : $"Native engine not loaded.\nLog file: {Logger.FilePath}";

        var dialog = new SettingsWindow(_settings, diagnostics) { Owner = this };
        if (dialog.ShowDialog() != true)
            return;

        var updated = dialog.Result;
        updated.Region = _settings.Region; // the region is not edited in this dialog
        _settings = updated;
        SettingsStore.Save(_settings);
        if (!_settings.PreviewEnabled)
        {
            PreviewImage.Source = null;
            _previewBitmap = null;
        }
        EnsureCaptureState();
    }

    // ------------------------------------------------------------------
    // Window placement
    // ------------------------------------------------------------------
    private void RestoreWindowPlacement()
    {
        if (_settings.WindowWidth is double w && _settings.WindowHeight is double h && w >= MinWidth && h >= MinHeight)
        {
            Width = w;
            Height = h;
        }
        if (_settings.WindowLeft is double left && _settings.WindowTop is double top &&
            left >= SystemParameters.VirtualScreenLeft - 50 && top >= SystemParameters.VirtualScreenTop - 50 &&
            left < SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 100 &&
            top < SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 100)
        {
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = left;
            Top = top;
        }
        else
        {
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
        }
    }

    private void SaveWindowPlacement()
    {
        var bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, ActualWidth, ActualHeight) : RestoreBounds;
        if (bounds.IsEmpty || double.IsNaN(bounds.Left))
            return;
        _settings.WindowLeft = bounds.Left;
        _settings.WindowTop = bounds.Top;
        _settings.WindowWidth = bounds.Width;
        _settings.WindowHeight = bounds.Height;
    }
}
