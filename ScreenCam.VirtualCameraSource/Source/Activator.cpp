// ScreenCam Virtual Camera Source - activation object.
#include "Activator.h"
#include "MediaSource.h"

HRESULT Activator::CreateInstance(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    Activator* activator = new (std::nothrow) Activator();
    if (!activator)
        return E_OUTOFMEMORY;
    HRESULT hr = activator->Initialize();
    if (SUCCEEDED(hr))
        hr = activator->QueryInterface(riid, ppv);
    activator->Release();
    return hr;
}

HRESULT Activator::Initialize()
{
    return MFCreateAttributes(&m_attributes, 4);
}

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate))
    {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    AutoLock lock(m_lock);
    if (!m_source)
    {
        ComPtr<IMFMediaSource> source;
        SC_RETURN_IF_FAILED(MediaSource::CreateInstance(m_attributes.Get(), IID_PPV_ARGS(&source)));
        m_source = source;
    }
    return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP Activator::ShutdownObject()
{
    AutoLock lock(m_lock);
    if (m_source)
    {
        m_source->Shutdown();
        m_source.Reset();
    }
    return S_OK;
}

STDMETHODIMP Activator::DetachObject()
{
    AutoLock lock(m_lock);
    m_source.Reset();
    return S_OK;
}
