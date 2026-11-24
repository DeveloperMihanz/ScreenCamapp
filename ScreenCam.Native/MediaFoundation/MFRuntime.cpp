// ScreenCam.Native - Media Foundation runtime lifetime.
#include "MFRuntime.h"
#include <mfapi.h>

namespace screencam
{
    namespace
    {
        std::mutex g_mfLock;
        int g_mfRefs = 0;
    }

    HRESULT MFRuntime::Startup()
    {
        std::lock_guard<std::mutex> lock(g_mfLock);
        if (g_mfRefs == 0)
        {
            const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (FAILED(hr))
            {
                SetLastErrorMessage(L"MFStartup failed: %s", DescribeHResult(hr).c_str());
                return hr;
            }
        }
        ++g_mfRefs;
        return S_OK;
    }

    void MFRuntime::Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mfLock);
        if (g_mfRefs > 0 && --g_mfRefs == 0)
            MFShutdown();
    }

    bool MFRuntime::IsStarted()
    {
        std::lock_guard<std::mutex> lock(g_mfLock);
        return g_mfRefs > 0;
    }
}
