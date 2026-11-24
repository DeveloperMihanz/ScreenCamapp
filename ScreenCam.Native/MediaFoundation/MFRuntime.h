// ScreenCam.Native - Media Foundation runtime lifetime (MFStartup / MFShutdown).
#pragma once

#include "NativeCommon.h"

namespace screencam
{
    class MFRuntime
    {
    public:
        static HRESULT Startup();   // reference counted
        static void Shutdown();
        static bool IsStarted();
    };
}
