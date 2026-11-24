// ScreenCam Virtual Camera Source - DLL entry points, class factory and COM registration.
#include "Common.h"
#include "Activator.h"
#include "MediaSource.h"

HMODULE g_hModule = nullptr;
std::atomic<long> g_objectCount{ 0 };
std::atomic<long> g_lockCount{ 0 };

namespace
{
    class ClassFactory final : public IClassFactory
    {
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
            {
                *ppv = static_cast<IClassFactory*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        // Static object: lifetime tied to the module.
        STDMETHODIMP_(ULONG) AddRef() override { return 2; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }

        STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (pUnkOuter)
                return CLASS_E_NOAGGREGATION;

            // The Frame Server asks for IMFActivate. Direct requests for the media
            // source are honoured too, for tools that create the source themselves.
            if (riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
                return MediaSource::CreateInstance(nullptr, riid, ppv);
            return Activator::CreateInstance(riid, ppv);
        }

        STDMETHODIMP LockServer(BOOL fLock) override
        {
            if (fLock) ++g_lockCount; else --g_lockCount;
            return S_OK;
        }
    };

    ClassFactory g_classFactory;

    HRESULT SetRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* name, const wchar_t* value)
    {
        HKEY key = nullptr;
        LSTATUS st = RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                     KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
        if (st != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(st);
        st = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                            (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(st);
    }

    const wchar_t kClsidKey[] = L"SOFTWARE\\Classes\\CLSID\\" SCREENCAM_SOURCE_CLSID_STRING;
    const wchar_t kInprocKey[] = L"SOFTWARE\\Classes\\CLSID\\" SCREENCAM_SOURCE_CLSID_STRING L"\\InprocServer32";
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_ScreenCamMediaSource)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_classFactory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return (g_objectCount.load() == 0 && g_lockCount.load() == 0) ? S_OK : S_FALSE;
}

// Registers the media source under HKLM so that the Frame Server service
// (running as LOCAL SERVICE) can find it. Requires an elevated caller (regsvr32).
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD len = GetModuleFileNameW(g_hModule, path, _countof(path));
    if (len == 0 || len >= _countof(path))
        return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = SetRegistryString(HKEY_LOCAL_MACHINE, kClsidKey, nullptr, SCREENCAM_SOURCE_DESCRIPTION);
    if (SUCCEEDED(hr))
        hr = SetRegistryString(HKEY_LOCAL_MACHINE, kInprocKey, nullptr, path);
    if (SUCCEEDED(hr))
        hr = SetRegistryString(HKEY_LOCAL_MACHINE, kInprocKey, L"ThreadingModel", L"Both");
    return hr;
}

STDAPI DllUnregisterServer()
{
    const LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND)
        return S_OK;
    return HRESULT_FROM_WIN32(st);
}
