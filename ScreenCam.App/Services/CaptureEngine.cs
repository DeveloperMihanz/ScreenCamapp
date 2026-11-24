using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ScreenCam.App.Models;
using ScreenCam.App.Settings;

namespace ScreenCam.App.Services;

/// <summary>Result of a native operation.</summary>
public readonly record struct EngineResult(bool Success, int Code, string Message)
{
    public static EngineResult Ok() => new(true, NativeMethods.SC_OK, string.Empty);
}

/// <summary>Snapshot of the native pipeline state.</summary>
public sealed record EngineStatus(
    bool CaptureRunning,
    bool CameraActive,
    bool ConnectedToCamera,
    bool ConsumerStreaming,
    int ConsumerWidth,
    int ConsumerHeight,
    double ConsumerFps,
    string ConsumerFormat,
    bool GpuProcessing,
    long FramesCaptured,
    long FramesPublished,
    long ConsumerFrames,
    double CaptureFps,
    bool CaptureLost);

/// <summary>
/// Facade over ScreenCam.Native.dll. All members must be called on the UI thread.
/// Owns exactly one capture session and at most one virtual camera, so the app
/// never creates duplicate capture sessions or cameras.
/// </summary>
public sealed class CaptureEngine : IDisposable
{
    private bool _initialized;
    private long _previewSequence;

    public bool IsVirtualCameraSupported { get; private set; }
    public bool IsCaptureSupported { get; private set; }
    public int WindowsBuild { get; private set; }

    public bool IsCaptureRunning { get; private set; }
    public bool IsCameraActive => _initialized && NativeMethods.SC_IsVirtualCameraActive() == 1;

    /// <summary>Loads the native library. Returns false with a message if it is missing or fails.</summary>
    public EngineResult Initialize()
    {
        try
        {
            int rc = NativeMethods.SC_Initialize();
            if (rc != NativeMethods.SC_OK)
                return Fail(rc);
            _initialized = true;
            IsVirtualCameraSupported = NativeMethods.SC_IsVirtualCameraSupported() == 1;
            IsCaptureSupported = NativeMethods.SC_IsCaptureSupported() == 1;
            WindowsBuild = NativeMethods.SC_GetWindowsBuild();
            Logger.Info($"Native engine initialized. Windows build {WindowsBuild}, virtual camera API: {IsVirtualCameraSupported}, capture: {IsCaptureSupported}");
            return EngineResult.Ok();
        }
        catch (DllNotFoundException ex)
        {
            Logger.Error("ScreenCam.Native.dll not found", ex);
            return new EngineResult(false, NativeMethods.SC_E_FAIL,
                "ScreenCam.Native.dll could not be loaded. Reinstall ScreenCam, or build the native projects (x64) before running from Visual Studio.");
        }
        catch (BadImageFormatException ex)
        {
            Logger.Error("ScreenCam.Native.dll has the wrong architecture", ex);
            return new EngineResult(false, NativeMethods.SC_E_FAIL, "ScreenCam.Native.dll is not an x64 library.");
        }
        catch (EntryPointNotFoundException ex)
        {
            Logger.Error("ScreenCam.Native.dll is outdated", ex);
            return new EngineResult(false, NativeMethods.SC_E_FAIL, "ScreenCam.Native.dll does not match this version of ScreenCam.");
        }
    }

    /// <summary>Re-checked on demand because the installer may register the component while the app runs.</summary>
    public bool IsSourceRegistered => _initialized && NativeMethods.SC_IsSourceRegistered() == 1;

    /// <summary>Starts capture or updates the running capture with new settings.</summary>
    public EngineResult ApplyCapture(PixelRect region, AppSettings settings)
    {
        if (!_initialized)
            return new EngineResult(false, NativeMethods.SC_E_NOT_INITIALIZED, "The native engine is not initialized.");

        var config = new NativeMethods.SC_CaptureConfig
        {
            RegionLeft = region.Left,
            RegionTop = region.Top,
            RegionWidth = region.Width,
            RegionHeight = region.Height,
            OutputWidth = settings.OutputWidth,
            OutputHeight = settings.OutputHeight,
            Fps = settings.FrameRate,
            ShowCursor = settings.ShowCursor ? 1 : 0,
            ScaleMode = (int)settings.ScaleMode,
            PreviewEnabled = settings.PreviewEnabled ? 1 : 0,
            PreviewMaxWidth = settings.PreviewMaxWidth,
            HideCaptureBorder = settings.HideCaptureBorder ? 1 : 0,
        };

        int rc = IsCaptureRunning
            ? NativeMethods.SC_UpdateCaptureConfig(ref config)
            : NativeMethods.SC_StartCapture(ref config);
        if (rc != NativeMethods.SC_OK)
        {
            IsCaptureRunning = false;
            return Fail(rc);
        }
        IsCaptureRunning = true;
        return EngineResult.Ok();
    }

    public void StopCapture()
    {
        if (!_initialized)
            return;
        NativeMethods.SC_StopCapture();
        IsCaptureRunning = false;
        _previewSequence = 0;
    }

    public EngineResult StartCamera()
    {
        if (!_initialized)
            return new EngineResult(false, NativeMethods.SC_E_NOT_INITIALIZED, "The native engine is not initialized.");
        int rc = NativeMethods.SC_StartVirtualCamera();
        if (rc != NativeMethods.SC_OK)
            return Fail(rc);
        Logger.Info("Virtual camera started");
        return EngineResult.Ok();
    }

    public void StopCamera()
    {
        if (!_initialized)
            return;
        NativeMethods.SC_StopVirtualCamera();
        Logger.Info("Virtual camera stopped");
    }

    public EngineStatus? GetStatus()
    {
        if (!_initialized || NativeMethods.SC_GetStatus(out var s) != NativeMethods.SC_OK)
            return null;
        double fps = s.ConsumerFpsDen > 0 ? (double)s.ConsumerFpsNum / s.ConsumerFpsDen : 0;
        string format = s.ConsumerFormat switch { 1 => "NV12", 2 => "RGB32", _ => "" };
        IsCaptureRunning = s.CaptureRunning == 1;
        return new EngineStatus(
            s.CaptureRunning == 1, s.CameraActive == 1, s.SharedConnected == 1, s.ConsumerStreaming == 1,
            s.ConsumerWidth, s.ConsumerHeight, fps, format, s.GpuProcessing == 1,
            s.FramesCaptured, s.FramesPublished, s.ConsumerFrames, s.CaptureFps, s.CaptureLost == 1);
    }

    /// <summary>
    /// Copies the newest preview frame into <paramref name="bitmap"/>, recreating it
    /// when the preview size changes. Returns true if the image changed.
    /// </summary>
    public bool UpdatePreview(ref WriteableBitmap? bitmap)
    {
        if (!_initialized || !IsCaptureRunning)
            return false;

        for (int attempt = 0; attempt < 2; attempt++)
        {
            int rc;
            int width, height;
            if (bitmap is null)
            {
                rc = NativeMethods.SC_GetPreviewFrame(IntPtr.Zero, 0, 0, out width, out height, ref _previewSequence);
            }
            else
            {
                bitmap.Lock();
                try
                {
                    rc = NativeMethods.SC_GetPreviewFrame(bitmap.BackBuffer, bitmap.BackBufferStride,
                        bitmap.BackBufferStride * bitmap.PixelHeight, out width, out height, ref _previewSequence);
                    if (rc == 1)
                    {
                        if (width == bitmap.PixelWidth && height == bitmap.PixelHeight)
                        {
                            bitmap.AddDirtyRect(new Int32Rect(0, 0, width, height));
                            return true;
                        }
                        rc = NativeMethods.SC_E_BUFFER_TOO_SMALL; // size changed: reallocate below
                    }
                }
                finally
                {
                    bitmap.Unlock();
                }
            }

            if (rc == NativeMethods.SC_E_BUFFER_TOO_SMALL || (rc == 0 && bitmap is null && width > 0))
            {
                bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgra32, null);
                _previewSequence = 0; // force a copy into the new bitmap
                continue;
            }
            return false;
        }
        return false;
    }

    private static EngineResult Fail(int code)
    {
        var message = NativeMethods.GetLastErrorMessage();
        if (string.IsNullOrWhiteSpace(message))
            message = $"Native error {code}.";
        Logger.Warn($"Native call failed ({code}): {message}");
        return new EngineResult(false, code, message);
    }

    public void Dispose()
    {
        if (!_initialized)
            return;
        try
        {
            NativeMethods.SC_Shutdown();
        }
        catch (Exception ex)
        {
            Logger.Error("Native shutdown failed", ex);
        }
        _initialized = false;
        IsCaptureRunning = false;
    }
}
