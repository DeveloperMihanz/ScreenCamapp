// ScreenCam Virtual Camera Source - media source implementation.
#include "MediaSource.h"
#include "MediaStream.h"

HRESULT MediaSource::CreateInstance(IMFAttributes* activationAttributes, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    MediaSource* source = new (std::nothrow) MediaSource();
    if (!source)
        return E_OUTOFMEMORY;
    HRESULT hr = source->Initialize(activationAttributes);
    if (SUCCEEDED(hr))
        hr = source->QueryInterface(riid, ppv);
    if (FAILED(hr))
        source->Shutdown();
    source->Release();
    return hr;
}

MediaSource::~MediaSource()
{
    Shutdown();
}

HRESULT MediaSource::Initialize(IMFAttributes* activationAttributes)
{
    SC_RETURN_IF_FAILED(MFCreateEventQueue(&m_eventQueue));
    SC_RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
    if (activationAttributes)
        SC_RETURN_IF_FAILED(activationAttributes->CopyAllItems(m_attributes.Get()));

    SC_RETURN_IF_FAILED(MediaStream::CreateInstance(this, kStreamId, &m_stream));

    ComPtr<IMFStreamDescriptor> descriptor;
    SC_RETURN_IF_FAILED(m_stream->GetStreamDescriptor(&descriptor));
    IMFStreamDescriptor* descriptors[] = { descriptor.Get() };
    SC_RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, &m_presentationDescriptor));
    SC_RETURN_IF_FAILED(m_presentationDescriptor->SelectStream(0));
    SCTrace(L"Media source created");
    return S_OK;
}

// --- IUnknown ---------------------------------------------------------------
STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    else if (riid == __uuidof(IMFGetService))
        *ppv = static_cast<IMFGetService*>(this);
    else if (riid == __uuidof(ISCKsControl))
        *ppv = static_cast<ISCKsControl*>(this);
    else if (riid == __uuidof(IMFSampleAllocatorControl))
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

// --- IMFMediaEventGenerator -------------------------------------------------
STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(m_lock);
        SC_RETURN_IF_FAILED(CheckShutdown());
        queue = m_eventQueue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// --- IMFMediaSource ---------------------------------------------------------
STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor)
{
    if (!ppPresentationDescriptor)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_presentationDescriptor->Clone(ppPresentationDescriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if (!pdwCharacteristics)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause()
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return MF_E_INVALID_STATE_TRANSITION; // live camera sources do not pause
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat,
                                const PROPVARIANT* pvarStartPosition)
{
    if (!pPresentationDescriptor)
        return E_INVALIDARG;
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());

    DWORD count = 0;
    SC_RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorCount(&count));
    for (DWORD i = 0; i < count; ++i)
    {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> descriptor;
        SC_RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorByIndex(i, &selected, &descriptor));
        DWORD id = 0;
        SC_RETURN_IF_FAILED(descriptor->GetStreamIdentifier(&id));
        if (id != kStreamId)
            return MF_E_INVALIDSTREAMNUMBER;

        if (selected)
        {
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            SC_RETURN_IF_FAILED(descriptor->GetMediaTypeHandler(&handler));
            SC_RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

            SC_RETURN_IF_FAILED(m_eventQueue->QueueEventParamUnk(m_streamActivated ? MEUpdatedStream : MENewStream,
                GUID_NULL, S_OK, static_cast<IMFMediaStream*>(m_stream.Get())));
            m_streamActivated = true;
            m_presentationDescriptor->SelectStream(i);
            SC_RETURN_IF_FAILED(m_stream->Start(type.Get()));
        }
        else
        {
            m_presentationDescriptor->DeselectStream(i);
            if (m_streamActivated)
                m_stream->Stop();
        }
    }

    PROPVARIANT start;
    PropVariantInit(&start);
    if (pvarStartPosition && pvarStartPosition->vt != VT_EMPTY)
    {
        PropVariantCopy(&start, pvarStartPosition);
    }
    else
    {
        start.vt = VT_I8;
        start.hVal.QuadPart = MFGetSystemTime();
    }
    HRESULT hr = m_eventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &start);
    PropVariantClear(&start);
    return hr;
}

STDMETHODIMP MediaSource::Stop()
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (m_streamActivated)
        m_stream->Stop();
    return m_eventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Shutdown()
{
    ComPtr<MediaStream> stream;
    {
        AutoLock lock(m_lock);
        if (m_isShutdown)
            return S_OK;
        m_isShutdown = true;
        if (m_eventQueue)
            m_eventQueue->Shutdown();
        stream = m_stream;
        m_stream.Reset();
        m_presentationDescriptor.Reset();
    }
    // Shut the stream down outside our lock (it joins its delivery thread).
    if (stream)
        stream->Shutdown();
    SCTrace(L"Media source shut down");
    return S_OK;
}

// --- IMFMediaSourceEx -------------------------------------------------------
STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_attributes.CopyTo(ppAttributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (dwStreamIdentifier != kStreamId)
        return MF_E_INVALIDSTREAMNUMBER;
    return m_stream->GetAttributes(ppAttributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown* /*pManager*/)
{
    // Samples come either from the allocator supplied through
    // IMFSampleAllocatorControl (which carries its own device) or from system
    // memory, so the DXGI device manager is not needed.
    AutoLock lock(m_lock);
    return CheckShutdown();
}

// --- IMFGetService ----------------------------------------------------------
STDMETHODIMP MediaSource::GetService(REFGUID /*guidService*/, REFIID /*riid*/, LPVOID* ppvObject)
{
    if (ppvObject)
        *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// --- IKsControl -------------------------------------------------------------
// The virtual camera exposes no KS property sets (no exposure, focus, etc.).
STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// --- IMFSampleAllocatorControl ----------------------------------------------
STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (dwOutputStreamID != kStreamId)
        return MF_E_INVALIDSTREAMNUMBER;
    return m_stream->SetAllocator(pAllocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage)
{
    if (!pdwInputStreamID || !peUsage)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (dwOutputStreamID != kStreamId)
        return MF_E_INVALIDSTREAMNUMBER;
    *pdwInputStreamID = dwOutputStreamID;
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}
