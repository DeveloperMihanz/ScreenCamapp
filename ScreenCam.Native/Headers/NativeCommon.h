// ScreenCam.Native - shared includes and diagnostics helpers.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <unknwn.h>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace screencam
{
    // Stores a human-readable error for SC_GetLastErrorMessage (thread-safe).
    void SetLastErrorMessage(const wchar_t* fmt, ...);
    std::wstring GetLastErrorMessageText();

    // "0x80070005 (Access is denied.)"
    std::wstring DescribeHResult(HRESULT hr);

    // Debug output, visible in Visual Studio / DebugView.
    void Trace(const wchar_t* fmt, ...);

    // Windows build number (e.g. 22631), read via RtlGetVersion.
    uint32_t GetWindowsBuildNumber();
}
