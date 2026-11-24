// ScreenCam.Native - flat C API consumed by the WPF application (P/Invoke).
//
// All functions use the C calling convention and are safe to call from the UI
// thread. Return values: SC_OK (0) on success, negative SC_E_* codes on failure.
// Call SC_GetLastErrorMessage() for a human-readable description of the last failure.
#pragma once

#include <stdint.h>

#ifdef SCREENCAM_NATIVE_EXPORTS
#define SC_API extern "C" __declspec(dllexport)
#else
#define SC_API extern "C" __declspec(dllimport)
#endif

#define SC_OK                      0
#define SC_E_FAIL                 -1
#define SC_E_INVALIDARG           -2
#define SC_E_NOT_INITIALIZED      -3
#define SC_E_UNSUPPORTED_OS       -4
#define SC_E_CAPTURE_UNSUPPORTED  -5
#define SC_E_SOURCE_NOT_REGISTERED -6
#define SC_E_CAMERA_FAILED        -7
#define SC_E_ACCESS_DENIED        -8
#define SC_E_BUFFER_TOO_SMALL     -9
#define SC_E_NO_MONITOR          -10

#pragma pack(push, 8)
typedef struct SC_CaptureConfig
{
    int32_t regionLeft;      // virtual-screen coordinates, physical pixels
    int32_t regionTop;
    int32_t regionWidth;
    int32_t regionHeight;
    int32_t outputWidth;     // camera frame size (even numbers, <= 1920x1080)
    int32_t outputHeight;
    int32_t fps;             // 30 or 60 (1..60 accepted)
    int32_t showCursor;      // 1 = draw the mouse cursor
    int32_t scaleMode;       // 0 = fit (letterbox), 1 = stretch, 2 = fill (crop)
    int32_t previewEnabled;  // 1 = produce preview frames for the UI
    int32_t previewMaxWidth; // preview width limit in pixels (e.g. 640)
    int32_t hideCaptureBorder; // 1 = ask Windows to hide the yellow capture border (Windows 11)
} SC_CaptureConfig;

typedef struct SC_Status
{
    int32_t captureRunning;     // capture session active
    int32_t cameraActive;       // virtual camera registered and started
    int32_t sharedConnected;    // connected to the media source's shared memory
    int32_t consumerStreaming;  // an application is currently streaming from the camera
    int32_t consumerWidth;
    int32_t consumerHeight;
    int32_t consumerFpsNum;
    int32_t consumerFpsDen;
    int32_t consumerFormat;     // 1 = NV12, 2 = RGB32
    int32_t gpuProcessing;      // 1 = D3D11 video processor path, 0 = CPU fallback
    int64_t framesCaptured;     // frames received from Windows Graphics Capture
    int64_t framesPublished;    // frames written to the camera
    int64_t consumerFrames;     // frames delivered by the media source to apps
    double  captureFps;         // measured capture frame rate (last second)
    int32_t captureLost;        // 1 = capture item closed (monitor removed etc.)
    int32_t reserved;
} SC_Status;
#pragma pack(pop)

// Library lifetime
SC_API int32_t SC_Initialize(void);
SC_API void    SC_Shutdown(void);

// Capability checks
SC_API int32_t SC_IsVirtualCameraSupported(void);   // 1 if MFCreateVirtualCamera exists (Windows 11+)
SC_API int32_t SC_IsCaptureSupported(void);         // 1 if Windows Graphics Capture is available
SC_API int32_t SC_IsSourceRegistered(void);         // 1 if the media source COM class is registered in HKLM
SC_API int32_t SC_GetWindowsBuild(void);

// Screen capture
SC_API int32_t SC_StartCapture(const SC_CaptureConfig* config);
SC_API int32_t SC_UpdateCaptureConfig(const SC_CaptureConfig* config);
SC_API void    SC_StopCapture(void);

// Preview: copies the newest preview frame (BGRA, top-down) into 'buffer'.
// *sequence is in/out: pass the last sequence you received; returns 1 when a
// newer frame was copied, 0 when nothing new, SC_E_BUFFER_TOO_SMALL when the
// buffer cannot hold *width x *height (both are set so the caller can resize).
SC_API int32_t SC_GetPreviewFrame(void* buffer, int32_t stride, int32_t capacity,
                                  int32_t* width, int32_t* height, int64_t* sequence);

// Virtual camera
SC_API int32_t SC_StartVirtualCamera(void);
SC_API int32_t SC_StopVirtualCamera(void);
SC_API int32_t SC_IsVirtualCameraActive(void);

// Diagnostics
SC_API int32_t SC_GetStatus(SC_Status* status);
SC_API int32_t SC_GetLastErrorMessage(wchar_t* buffer, int32_t capacity);
