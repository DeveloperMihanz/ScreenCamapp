// ScreenCam.Native - registers / starts / removes the virtual camera with
// MFCreateVirtualCamera (Windows 11, build 22000+).
#pragma once

#include "NativeCommon.h"

struct IMFVirtualCamera;

namespace screencam
{
    class VirtualCameraController
    {
    public:
        VirtualCameraController() = default;
        ~VirtualCameraController();
        VirtualCameraController(const VirtualCameraController&) = delete;
        VirtualCameraController& operator=(const VirtualCameraController&) = delete;

        static bool IsSupported();          // MFCreateVirtualCamera exported by mfsensorgroup.dll
        static bool IsSourceRegistered();   // our CLSID present in HKLM with an existing DLL path

        int32_t Start();                    // returns SC_OK or SC_E_*
        int32_t Stop();
        bool IsActive() const { return m_active.load(); }

    private:
        std::mutex m_lock;
        IMFVirtualCamera* m_camera = nullptr;
        std::atomic<bool> m_active{ false };
    };
}
