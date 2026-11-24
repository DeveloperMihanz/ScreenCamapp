// ScreenCam Virtual Camera Source - common includes and helpers.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <ks.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <new>
#include <initializer_list>

#include "ScreenCamIds.h"

using Microsoft::WRL::ComPtr;

extern HMODULE g_hModule;
extern std::atomic<long> g_objectCount;
extern std::atomic<long> g_lockCount;

// KS pin category for a video capture pin ("PINNAME_VIDEO_CAPTURE").
// {FB6C4281-0353-11D1-905F-0000C0CC16BA}
static const GUID SC_PINNAME_VIDEO_CAPTURE =
{ 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

// IKsControl, declared locally (identical ABI to the one in ksproxy.h) so that
// we do not depend on DirectShow proxy headers. The Frame Server queries this
// interface on device sources; we report that no KS property sets are supported.
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
ISCKsControl : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
        LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD Method, ULONG MethodLength,
        LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT Event, ULONG EventLength,
        LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) = 0;
};

// Debug tracing (visible with DebugView / a debugger attached to the Frame Server).
inline void SCTrace(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    int n = swprintf_s(buf, L"[ScreenCamSource] ");
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf + n, _countof(buf) - n, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

#define SC_RETURN_IF_FAILED(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) { SCTrace(L"%S failed 0x%08X (%S:%d)", #expr, _hr, __FILE__, __LINE__); return _hr; } } while (0)

// Small helper for implementing IUnknown reference counting.
class RefCounted
{
protected:
    RefCounted() { ++g_objectCount; }
    virtual ~RefCounted() { --g_objectCount; }
    ULONG InternalAddRef() { return (ULONG)++m_ref; }
    ULONG InternalRelease()
    {
        const long r = --m_ref;
        if (r == 0)
            delete this;
        return (ULONG)r;
    }
private:
    std::atomic<long> m_ref{ 1 };
};

// Scoped CRITICAL_SECTION lock (recursive, which Media Foundation callbacks need).
class CritSec
{
public:
    CritSec() { InitializeCriticalSectionEx(&m_cs, 4000, 0); }
    ~CritSec() { DeleteCriticalSection(&m_cs); }
    void Lock() { EnterCriticalSection(&m_cs); }
    void Unlock() { LeaveCriticalSection(&m_cs); }
    CritSec(const CritSec&) = delete;
    CritSec& operator=(const CritSec&) = delete;
private:
    CRITICAL_SECTION m_cs;
};

class AutoLock
{
public:
    explicit AutoLock(CritSec& cs) : m_cs(cs) { m_cs.Lock(); }
    ~AutoLock() { m_cs.Unlock(); }
private:
    CritSec& m_cs;
};
