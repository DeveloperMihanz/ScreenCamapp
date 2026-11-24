using System.Runtime.InteropServices;

namespace ScreenCam.App.Services;

/// <summary>P/Invoke declarations for ScreenCam.Native.dll (see ScreenCam.Native/Headers/ScreenCamNative.h).</summary>
internal static class NativeMethods
{
    private const string Dll = "ScreenCam.Native.dll";

    public const int SC_OK = 0;
    public const int SC_E_FAIL = -1;
    public const int SC_E_INVALIDARG = -2;
    public const int SC_E_NOT_INITIALIZED = -3;
    public const int SC_E_UNSUPPORTED_OS = -4;
    public const int SC_E_CAPTURE_UNSUPPORTED = -5;
    public const int SC_E_SOURCE_NOT_REGISTERED = -6;
    public const int SC_E_CAMERA_FAILED = -7;
    public const int SC_E_ACCESS_DENIED = -8;
    public const int SC_E_BUFFER_TOO_SMALL = -9;
    public const int SC_E_NO_MONITOR = -10;

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct SC_CaptureConfig
    {
        public int RegionLeft;
        public int RegionTop;
        public int RegionWidth;
        public int RegionHeight;
        public int OutputWidth;
        public int OutputHeight;
        public int Fps;
        public int ShowCursor;
        public int ScaleMode;
        public int PreviewEnabled;
        public int PreviewMaxWidth;
        public int HideCaptureBorder;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct SC_Status
    {
        public int CaptureRunning;
        public int CameraActive;
        public int SharedConnected;
        public int ConsumerStreaming;
        public int ConsumerWidth;
        public int ConsumerHeight;
        public int ConsumerFpsNum;
        public int ConsumerFpsDen;
        public int ConsumerFormat;
        public int GpuProcessing;
        public long FramesCaptured;
        public long FramesPublished;
        public long ConsumerFrames;
        public double CaptureFps;
        public int CaptureLost;
        public int Reserved;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_Initialize();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void SC_Shutdown();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_IsVirtualCameraSupported();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_IsCaptureSupported();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_IsSourceRegistered();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_GetWindowsBuild();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_StartCapture(ref SC_CaptureConfig config);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_UpdateCaptureConfig(ref SC_CaptureConfig config);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void SC_StopCapture();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_GetPreviewFrame(IntPtr buffer, int stride, int capacity,
        out int width, out int height, ref long sequence);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_StartVirtualCamera();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_StopVirtualCamera();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_IsVirtualCameraActive();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SC_GetStatus(out SC_Status status);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int SC_GetLastErrorMessage([Out] char[]? buffer, int capacity);

    public static string GetLastErrorMessage()
    {
        try
        {
            var buffer = new char[2048];
            int n = SC_GetLastErrorMessage(buffer, buffer.Length);
            return n > 0 ? new string(buffer, 0, Math.Min(n, buffer.Length - 1)) : string.Empty;
        }
        catch
        {
            return string.Empty;
        }
    }
}
