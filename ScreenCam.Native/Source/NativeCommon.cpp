// ScreenCam.Native - diagnostics helpers.
#include "NativeCommon.h"
#include <cstdarg>
#include <cwchar>

namespace screencam
{
    namespace
    {
        std::mutex g_errorLock;
        std::wstring g_lastError;

        std::wstring FormatV(const wchar_t* fmt, va_list args)
        {
            wchar_t buf[1024];
            vswprintf_s(buf, _countof(buf), fmt, args);
            return buf;
        }
    }

    void SetLastErrorMessage(const wchar_t* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::wstring text = FormatV(fmt, args);
        va_end(args);
        OutputDebugStringW((L"[ScreenCam.Native] ERROR: " + text + L"\n").c_str());
        std::lock_guard<std::mutex> lock(g_errorLock);
        g_lastError = std::move(text);
    }

    std::wstring GetLastErrorMessageText()
    {
        std::lock_guard<std::mutex> lock(g_errorLock);
        return g_lastError;
    }

    std::wstring DescribeHResult(HRESULT hr)
    {
        wchar_t code[16];
        swprintf_s(code, L"0x%08X", (unsigned)hr);
        std::wstring result = code;
        wchar_t* msg = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, (DWORD)hr, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        if (n && msg)
        {
            std::wstring text(msg, n);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
                text.pop_back();
            result += L" (" + text + L")";
        }
        if (msg)
            LocalFree(msg);
        return result;
    }

    void Trace(const wchar_t* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::wstring text = FormatV(fmt, args);
        va_end(args);
        OutputDebugStringW((L"[ScreenCam.Native] " + text + L"\n").c_str());
    }

    uint32_t GetWindowsBuildNumber()
    {
        typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
        static uint32_t cached = 0;
        if (cached)
            return cached;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
        {
            auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn)
            {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0)
                    cached = info.dwBuildNumber;
            }
        }
        return cached;
    }
}
