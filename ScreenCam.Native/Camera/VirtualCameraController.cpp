// ScreenCam.Native - virtual camera lifetime via the Windows 11 Virtual Camera API.
#include "VirtualCameraController.h"
#include "MFRuntime.h"
#include "ScreenCamIds.h"
#include "ScreenCamNative.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

namespace screencam
{
    namespace
    {
        using CreateVirtualCameraFn = decltype(&MFCreateVirtualCamera);

        CreateVirtualCameraFn LoadCreateVirtualCamera()
        {
            // Loaded dynamically so that the application still starts on Windows 10
            // (where the API does not exist) and can show a clear message instead.
            static CreateVirtualCameraFn fn = []() -> CreateVirtualCameraFn
            {
                HMODULE module = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!module)
                    return nullptr;
                return reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(module, "MFCreateVirtualCamera"));
            }();
            return fn;
        }

        const wchar_t kInprocKey[] = L"SOFTWARE\\Classes\\CLSID\\" SCREENCAM_SOURCE_CLSID_STRING L"\\InprocServer32";
    }

    VirtualCameraController::~VirtualCameraController()
    {
        Stop();
    }

    bool VirtualCameraController::IsSupported()
    {
        // The Virtual Camera API shipped with Windows 11 (build 22000).
        return GetWindowsBuildNumber() >= 22000 && LoadCreateVirtualCamera() != nullptr;
    }

    bool VirtualCameraController::IsSourceRegistered()
    {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = sizeof(path);
        const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, kInprocKey, nullptr,
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &size);
        if (st != ERROR_SUCCESS)
            return false;
        const DWORD attrs = GetFileAttributesW(path);
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    int32_t VirtualCameraController::Start()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_camera)
            return SC_OK; // already running: never create a duplicate camera

        if (!IsSupported())
        {
            SetLastErrorMessage(L"The Windows Virtual Camera API is not available. ScreenCam requires Windows 11 (build 22000 or later); this PC runs build %u.",
                                GetWindowsBuildNumber());
            return SC_E_UNSUPPORTED_OS;
        }
        if (!IsSourceRegistered())
        {
            SetLastErrorMessage(L"The ScreenCam camera component (ScreenCam.VirtualCameraSource.dll) is not registered. "
                                L"Run Scripts\\install.ps1 (or the installer) as Administrator.");
            return SC_E_SOURCE_NOT_REGISTERED;
        }
        HRESULT hr = MFRuntime::Startup();
        if (FAILED(hr))
            return SC_E_CAMERA_FAILED;

        IMFVirtualCamera* camera = nullptr;
        hr = LoadCreateVirtualCamera()(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,     // removed automatically when ScreenCam exits
            MFVirtualCameraAccess_CurrentUser,   // no elevation needed
            SCREENCAM_CAMERA_FRIENDLY_NAME,
            SCREENCAM_SOURCE_CLSID_STRING,
            nullptr, 0,                          // default categories (video camera)
            &camera);
        if (SUCCEEDED(hr))
        {
            hr = camera->Start(nullptr);         // synchronous start
            if (FAILED(hr))
            {
                camera->Remove();
                camera->Shutdown();
                camera->Release();
                camera = nullptr;
            }
        }

        if (FAILED(hr))
        {
            MFRuntime::Shutdown();
            if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
            {
                SetLastErrorMessage(L"Windows denied access to create the virtual camera (%s). Check Settings > Privacy & security > Camera: "
                                    L"\"Camera access\" and \"Let desktop apps access your camera\" must be On.",
                                    DescribeHResult(hr).c_str());
                return SC_E_ACCESS_DENIED;
            }
            if (hr == REGDB_E_CLASSNOTREG)
            {
                SetLastErrorMessage(L"The camera media source class is not registered (%s). Re-run the installer as Administrator.",
                                    DescribeHResult(hr).c_str());
                return SC_E_SOURCE_NOT_REGISTERED;
            }
            SetLastErrorMessage(L"Could not start the virtual camera: %s", DescribeHResult(hr).c_str());
            return SC_E_CAMERA_FAILED;
        }

        m_camera = camera;
        m_active = true;
        Trace(L"Virtual camera started");
        return SC_OK;
    }

    int32_t VirtualCameraController::Stop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_camera)
            return SC_OK;
        m_active = false;
        m_camera->Stop();
        m_camera->Remove();
        m_camera->Shutdown();
        m_camera->Release();
        m_camera = nullptr;
        MFRuntime::Shutdown();
        Trace(L"Virtual camera stopped and removed");
        return SC_OK;
    }
}
