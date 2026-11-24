// ScreenCam.Native - exported C API (see Headers/ScreenCamNative.h).
#include "NativeCommon.h"
#include "ScreenCamNative.h"
#include "ScreenCapture.h"
#include "VirtualCameraController.h"
#include "MFRuntime.h"

#include <memory>

using namespace screencam;

namespace
{
    std::mutex g_apiLock;
    std::unique_ptr<ScreenCapture> g_capture;
    std::unique_ptr<VirtualCameraController> g_camera;
    bool g_initialized = false;

    bool EnsureInitialized()
    {
        if (!g_initialized)
        {
            SetLastErrorMessage(L"ScreenCam.Native is not initialized (call SC_Initialize first).");
            return false;
        }
        return true;
    }
}

SC_API int32_t SC_Initialize(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (g_initialized)
        return SC_OK;
    try
    {
        // The WPF UI thread is already an STA; WinRT calls work from it as-is.
        if (FAILED(MFRuntime::Startup()))
            return SC_E_FAIL;
        g_capture = std::make_unique<ScreenCapture>();
        g_camera = std::make_unique<VirtualCameraController>();
        g_initialized = true;
        Trace(L"Initialized (Windows build %u)", GetWindowsBuildNumber());
        return SC_OK;
    }
    catch (...)
    {
        SetLastErrorMessage(L"Initialization failed (out of memory?).");
        return SC_E_FAIL;
    }
}

SC_API void SC_Shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!g_initialized)
        return;
    // Order matters: stop sending frames, remove the camera, then stop capture.
    if (g_capture)
        g_capture->SetCameraActive(false);
    if (g_camera)
        g_camera->Stop();
    if (g_capture)
        g_capture->Stop();
    g_camera.reset();
    g_capture.reset();
    MFRuntime::Shutdown();
    g_initialized = false;
    Trace(L"Shut down");
}

SC_API int32_t SC_IsVirtualCameraSupported(void)
{
    return VirtualCameraController::IsSupported() ? 1 : 0;
}

SC_API int32_t SC_IsCaptureSupported(void)
{
    return ScreenCapture::IsSupported() ? 1 : 0;
}

SC_API int32_t SC_IsSourceRegistered(void)
{
    return VirtualCameraController::IsSourceRegistered() ? 1 : 0;
}

SC_API int32_t SC_GetWindowsBuild(void)
{
    return (int32_t)GetWindowsBuildNumber();
}

SC_API int32_t SC_StartCapture(const SC_CaptureConfig* config)
{
    if (!config)
        return SC_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!EnsureInitialized())
        return SC_E_NOT_INITIALIZED;
    return g_capture->Start(*config);
}

SC_API int32_t SC_UpdateCaptureConfig(const SC_CaptureConfig* config)
{
    if (!config)
        return SC_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!EnsureInitialized())
        return SC_E_NOT_INITIALIZED;
    return g_capture->Update(*config);
}

SC_API void SC_StopCapture(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (g_initialized && g_capture)
        g_capture->Stop();
}

SC_API int32_t SC_GetPreviewFrame(void* buffer, int32_t stride, int32_t capacity,
                                  int32_t* width, int32_t* height, int64_t* sequence)
{
    // No API lock: the capture object guards its preview buffer itself and lives
    // until SC_Shutdown (which the app calls only after stopping its timers).
    if (!g_initialized || !g_capture)
        return SC_E_NOT_INITIALIZED;
    return g_capture->GetPreview(buffer, stride, capacity, width, height, sequence);
}

SC_API int32_t SC_StartVirtualCamera(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!EnsureInitialized())
        return SC_E_NOT_INITIALIZED;
    const int32_t rc = g_camera->Start();
    if (rc == SC_OK)
        g_capture->SetCameraActive(true);
    return rc;
}

SC_API int32_t SC_StopVirtualCamera(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!EnsureInitialized())
        return SC_E_NOT_INITIALIZED;
    g_capture->SetCameraActive(false);
    return g_camera->Stop();
}

SC_API int32_t SC_IsVirtualCameraActive(void)
{
    std::lock_guard<std::mutex> lock(g_apiLock);
    return (g_initialized && g_camera && g_camera->IsActive()) ? 1 : 0;
}

SC_API int32_t SC_GetStatus(SC_Status* status)
{
    if (!status)
        return SC_E_INVALIDARG;
    *status = SC_Status{};
    std::lock_guard<std::mutex> lock(g_apiLock);
    if (!EnsureInitialized())
        return SC_E_NOT_INITIALIZED;
    g_capture->FillStatus(*status);
    status->cameraActive = g_camera->IsActive() ? 1 : 0;
    return SC_OK;
}

SC_API int32_t SC_GetLastErrorMessage(wchar_t* buffer, int32_t capacity)
{
    const std::wstring text = GetLastErrorMessageText();
    if (!buffer || capacity <= 0)
        return (int32_t)text.size() + 1;
    wcsncpy_s(buffer, (size_t)capacity, text.c_str(), _TRUNCATE);
    return (int32_t)text.size();
}
