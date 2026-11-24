// ScreenCam Virtual Camera Source - video stream implementation.
#include "MediaStream.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    struct ModeDesc { UINT32 width, height, fps; };

    // Advertised capture modes (prompt requirement: 640x480, 1280x720, 1920x1080 at 30/60 fps).
    const ModeDesc kModes[] =
    {
        { 1280, 720, 30 }, { 1920, 1080, 30 }, { 640, 480, 30 },
        { 1280, 720, 60 }, { 1920, 1080, 60 }, { 640, 480, 60 },
    };

    HRESULT CreateVideoType(const GUID& subtype, UINT32 width, UINT32 height, UINT32 fps, IMFMediaType** result)
    {
        ComPtr<IMFMediaType> type;
        SC_RETURN_IF_FAILED(MFCreateMediaType(&type));
        SC_RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        SC_RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype));
        SC_RETURN_IF_FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height));
        SC_RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1));
        SC_RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));

        UINT32 stride, size;
        if (subtype == MFVideoFormat_NV12)
        {
            stride = width;
            size = width * height * 3 / 2;
            SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
            SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
            SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
            SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
        }
        else
        {
            stride = width * 4;
            size = width * height * 4;
        }
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride));
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, size));
        const UINT64 bitrate = (UINT64)size * 8 * fps;
        SC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::min<UINT64>(bitrate, 0xFFFFFFFFull)));
        *result = type.Detach();
        return S_OK;
    }
}

HRESULT MediaStream::CreateInstance(IMFMediaSource* source, DWORD streamId, MediaStream** stream)
{
    if (!stream)
        return E_POINTER;
    *stream = nullptr;
    MediaStream* s = new (std::nothrow) MediaStream();
    if (!s)
        return E_OUTOFMEMORY;
    HRESULT hr = s->Initialize(source, streamId);
    if (FAILED(hr))
    {
        s->Shutdown();
        s->Release();
        return hr;
    }
    *stream = s;
    return S_OK;
}

MediaStream::~MediaStream()
{
    Shutdown();
    if (m_wakeEvent) CloseHandle(m_wakeEvent);
    if (m_exitEvent) CloseHandle(m_exitEvent);
    if (m_timer) CloseHandle(m_timer);
}

HRESULT MediaStream::Initialize(IMFMediaSource* source, DWORD streamId)
{
    m_source = source;
    m_streamId = streamId;

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_exitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!m_timer)
        m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr); // pre-1803 fallback
    if (!m_wakeEvent || !m_exitEvent || !m_timer)
        return HRESULT_FROM_WIN32(GetLastError());

    SC_RETURN_IF_FAILED(MFCreateEventQueue(&m_eventQueue));

    // Stream attributes required by the Frame Server.
    SC_RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 8));
    SC_RETURN_IF_FAILED(m_attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, SC_PINNAME_VIDEO_CAPTURE));
    SC_RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
    SC_RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    SC_RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));

    std::vector<ComPtr<IMFMediaType>> types;
    SC_RETURN_IF_FAILED(BuildMediaTypes(types));
    std::vector<IMFMediaType*> raw;
    for (auto& t : types)
        raw.push_back(t.Get());

    SC_RETURN_IF_FAILED(MFCreateStreamDescriptor(streamId, (DWORD)raw.size(), raw.data(), &m_descriptor));
    ComPtr<IMFMediaTypeHandler> handler;
    SC_RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
    SC_RETURN_IF_FAILED(handler->SetCurrentMediaType(raw[0]));
    SC_RETURN_IF_FAILED(m_attributes->CopyAllItems(m_descriptor.Get()));
    SC_RETURN_IF_FAILED(ApplyMediaType(raw[0]));
    return S_OK;
}

HRESULT MediaStream::BuildMediaTypes(std::vector<ComPtr<IMFMediaType>>& types)
{
    std::vector<ModeDesc> modes(std::begin(kModes), std::end(kModes));

    // Put the mode selected in the desktop app first: many apps open the first
    // advertised media type by default.
    UINT32 pw = 0, ph = 0, pf = 0;
    if (m_reader.GetPreferredMode(&pw, &ph, &pf))
    {
        for (size_t i = 0; i < modes.size(); ++i)
        {
            if (modes[i].width == pw && modes[i].height == ph && modes[i].fps == pf)
            {
                std::rotate(modes.begin(), modes.begin() + (ptrdiff_t)i, modes.begin() + (ptrdiff_t)i + 1);
                break;
            }
        }
    }

    for (const GUID* subtype : { &MFVideoFormat_NV12, &MFVideoFormat_RGB32 })
    {
        for (const auto& m : modes)
        {
            ComPtr<IMFMediaType> type;
            SC_RETURN_IF_FAILED(CreateVideoType(*subtype, m.width, m.height, m.fps, &type));
            types.push_back(type);
        }
    }
    return S_OK;
}

HRESULT MediaStream::ApplyMediaType(IMFMediaType* mediaType)
{
    GUID subtype{};
    SC_RETURN_IF_FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype));
    UINT32 w = 0, h = 0, num = 0, den = 0;
    SC_RETURN_IF_FAILED(MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &w, &h));
    if (FAILED(MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &num, &den)) || num == 0 || den == 0)
    {
        num = 30;
        den = 1;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || (w & 1) || (h & 1))
        return MF_E_INVALIDMEDIATYPE;
    if (subtype == MFVideoFormat_NV12)
        m_format = OutputFormat::NV12;
    else if (subtype == MFVideoFormat_RGB32)
        m_format = OutputFormat::RGB32;
    else
        return MF_E_INVALIDMEDIATYPE;

    m_width = w;
    m_height = h;
    m_fpsNum = num;
    m_fpsDen = den;
    m_frameDuration = (LONGLONG)(10000000ll * den / num);
    m_currentType = mediaType;
    return S_OK;
}

// --- IUnknown ---------------------------------------------------------------
STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2))
    {
        *ppv = static_cast<IMFMediaStream2*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// --- IMFMediaEventGenerator -------------------------------------------------
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    // Must not hold the lock while blocking.
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(m_lock);
        SC_RETURN_IF_FAILED(CheckShutdown());
        queue = m_eventQueue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// --- IMFMediaStream ---------------------------------------------------------
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if (!ppMediaSource)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_source.CopyTo(ppMediaSource);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (!ppStreamDescriptor)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_descriptor.CopyTo(ppStreamDescriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (m_state != MF_STREAM_STATE_RUNNING)
        return MF_E_MEDIA_SOURCE_WRONGSTATE;
    if (m_tokens.size() > 64)
        return MF_E_NOTACCEPTING; // a well-behaved client never gets here
    m_tokens.emplace_back(pToken);
    SetEvent(m_wakeEvent);
    return S_OK;
}

// --- IMFMediaStream2 --------------------------------------------------------
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
    {
        AutoLock lock(m_lock);
        SC_RETURN_IF_FAILED(CheckShutdown());
        if (value == m_state)
            return S_OK;
    }
    switch (value)
    {
    case MF_STREAM_STATE_RUNNING:
        return Start(nullptr);
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    case MF_STREAM_STATE_PAUSED:
    {
        AutoLock lock(m_lock);
        m_state = MF_STREAM_STATE_PAUSED;
        m_tokens.clear();
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
    if (!value)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    *value = m_state;
    return S_OK;
}

// --- Control (called by MediaSource) ------------------------------------------
HRESULT MediaStream::Start(IMFMediaType* mediaType)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());

    ComPtr<IMFMediaType> type = mediaType;
    if (!type)
    {
        ComPtr<IMFMediaTypeHandler> handler;
        SC_RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
        SC_RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
    }
    SC_RETURN_IF_FAILED(ApplyMediaType(type.Get()));

    if (m_allocator)
    {
        if (m_allocatorInitialized)
        {
            m_allocator->UninitializeSampleAllocator();
            m_allocatorInitialized = false;
        }
        HRESULT hr = m_allocator->InitializeSampleAllocatorEx(4, 10, nullptr, type.Get());
        if (SUCCEEDED(hr))
            m_allocatorInitialized = true;
        else
        {
            SCTrace(L"InitializeSampleAllocatorEx failed 0x%08X - using system memory samples", hr);
            m_allocator.Reset();
        }
    }

    m_tokens.clear();
    m_nextDeadline = 0;
    m_state = MF_STREAM_STATE_RUNNING;

    if (!m_worker.joinable())
    {
        ResetEvent(m_exitEvent);
        try
        {
            m_worker = std::thread(&MediaStream::WorkerLoop, this);
        }
        catch (...)
        {
            m_state = MF_STREAM_STATE_STOPPED;
            return E_OUTOFMEMORY;
        }
    }

    SCTrace(L"Stream started %ux%u @ %u/%u fps (%s)", m_width, m_height, m_fpsNum, m_fpsDen,
            m_format == OutputFormat::NV12 ? L"NV12" : L"RGB32");
    return m_eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Stop()
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    m_state = MF_STREAM_STATE_STOPPED;
    m_tokens.clear();
    SetEvent(m_wakeEvent); // let the worker publish "not streaming"
    SCTrace(L"Stream stopped");
    return m_eventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    if (m_allocator && m_allocatorInitialized)
        m_allocator->UninitializeSampleAllocator();
    m_allocator.Reset();
    m_allocatorInitialized = false;
    if (allocator)
    {
        // Optional: if the Frame Server gives us a video sample allocator we use it,
        // otherwise samples are allocated in system memory.
        allocator->QueryInterface(IID_PPV_ARGS(&m_allocator));
    }
    return S_OK;
}

HRESULT MediaStream::GetAttributes(IMFAttributes** attributes)
{
    if (!attributes)
        return E_POINTER;
    AutoLock lock(m_lock);
    SC_RETURN_IF_FAILED(CheckShutdown());
    return m_attributes.CopyTo(attributes);
}

HRESULT MediaStream::Shutdown()
{
    {
        AutoLock lock(m_lock);
        if (m_isShutdown)
            return S_OK;
        m_isShutdown = true;
        m_state = MF_STREAM_STATE_STOPPED;
        m_tokens.clear();
        if (m_eventQueue)
            m_eventQueue->Shutdown();
    }

    StopWorker(); // outside the lock: the worker takes the lock too

    AutoLock lock(m_lock);
    if (m_allocator && m_allocatorInitialized)
        m_allocator->UninitializeSampleAllocator();
    m_allocator.Reset();
    m_allocatorInitialized = false;
    m_currentType.Reset();
    m_source.Reset();
    return S_OK;
}

void MediaStream::StopWorker()
{
    if (m_exitEvent)
        SetEvent(m_exitEvent);
    if (m_worker.joinable())
    {
        if (m_worker.get_id() == std::this_thread::get_id())
            m_worker.detach(); // cannot join ourselves (defensive; not expected)
        else
            m_worker.join();
    }
}

// --- Frame generation -------------------------------------------------------
HRESULT MediaStream::CreateSample(IMFSample** result)
{
    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    OutputFormat format;
    UINT32 width, height;
    {
        AutoLock lock(m_lock);
        if (m_allocatorInitialized)
            allocator = m_allocator;
        format = m_format;
        width = m_width;
        height = m_height;
    }

    ComPtr<IMFSample> sample;
    if (allocator)
    {
        if (FAILED(allocator->AllocateSample(&sample)))
            sample.Reset(); // pool exhausted: fall back to a system memory sample
    }
    if (!sample)
    {
        SC_RETURN_IF_FAILED(MFCreateSample(&sample));
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD fourcc = (format == OutputFormat::NV12) ? MFVideoFormat_NV12.Data1 : MFVideoFormat_RGB32.Data1;
        SC_RETURN_IF_FAILED(MFCreate2DMediaBuffer(width, height, fourcc, FALSE, &buffer));
        SC_RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
    }

    // Render
    ComPtr<IMFMediaBuffer> buffer;
    SC_RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));
    const DWORD frameBytes = (format == OutputFormat::NV12) ? width * height * 3 / 2 : width * height * 4;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        BYTE* scanline0 = nullptr;
        BYTE* bufferStart = nullptr;
        LONG pitch = 0;
        DWORD bufferLength = 0;
        SC_RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &bufferStart, &bufferLength));
        if (pitch > 0)
        {
            m_reader.Render(format, scanline0, pitch, width, height);
        }
        else
        {
            // Bottom-up layout (not expected with our positive MF_MT_DEFAULT_STRIDE):
            // render top-down into a scratch buffer, then copy rows.
            const LONG rowBytes = (LONG)(format == OutputFormat::NV12 ? width : width * 4);
            std::vector<uint8_t> scratch(frameBytes);
            m_reader.Render(format, scratch.data(), rowBytes, width, height);
            for (UINT32 row = 0; row < height; ++row)
                memcpy(scanline0 + (LONG_PTR)row * pitch, scratch.data() + (size_t)row * rowBytes, (size_t)rowBytes);
        }
        buffer2d->Unlock2D();
        DWORD contiguous = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&contiguous)))
            buffer->SetCurrentLength(contiguous);
    }
    else
    {
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        SC_RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
        if (maxLength >= frameBytes)
            m_reader.Render(format, data, (LONG)(format == OutputFormat::NV12 ? width : width * 4), width, height);
        buffer->Unlock();
        buffer->SetCurrentLength(frameBytes);
    }

    *result = sample.Detach();
    return S_OK;
}

void MediaStream::WorkerLoop()
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ULONGLONG lastReport = 0;
    bool exit = false;

    while (!exit)
    {
        HANDLE waits[2] = { m_exitEvent, m_wakeEvent };
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (wr == WAIT_OBJECT_0)
            break;

        for (;;)
        {
            ComPtr<IUnknown> token;
            LONGLONG duration;
            bool running;
            {
                AutoLock lock(m_lock);
                if (m_isShutdown) { exit = true; break; }
                running = (m_state == MF_STREAM_STATE_RUNNING);
                if (!running || m_tokens.empty())
                {
                    if (!running)
                        m_tokens.clear();
                    duration = 0;
                }
                else
                {
                    token = m_tokens.front();
                    m_tokens.pop_front();
                    duration = m_frameDuration;
                }
            }

            const ULONGLONG tick = GetTickCount64();
            if (tick - lastReport >= 500 || !running)
            {
                lastReport = tick;
                m_reader.EnsureOpen();
                m_reader.ReportConsumer(running, m_format, m_width, m_height, m_fpsNum, m_fpsDen, m_framesDelivered);
            }
            if (duration == 0)
                break; // nothing requested (or not running)

            // Pace delivery to the negotiated frame rate.
            LONGLONG now = MFGetSystemTime();
            if (m_nextDeadline == 0)
                m_nextDeadline = now;
            if (now < m_nextDeadline)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(m_nextDeadline - now); // relative, 100 ns units
                SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE timerWaits[2] = { m_exitEvent, m_timer };
                if (WaitForMultipleObjects(2, timerWaits, FALSE, INFINITE) == WAIT_OBJECT_0)
                {
                    exit = true;
                    break;
                }
                now = MFGetSystemTime();
            }
            if (now - m_nextDeadline > duration)
                m_nextDeadline = now + duration;  // fell behind: resynchronize
            else
                m_nextDeadline += duration;

            ComPtr<IMFSample> sample;
            HRESULT hr = CreateSample(&sample);
            if (SUCCEEDED(hr))
            {
                sample->SetSampleTime(now);
                sample->SetSampleDuration(duration);
                if (token)
                    sample->SetUnknown(MFSampleExtension_Token, token.Get());

                AutoLock lock(m_lock);
                if (!m_isShutdown && m_state == MF_STREAM_STATE_RUNNING)
                {
                    hr = m_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
                    if (SUCCEEDED(hr))
                        ++m_framesDelivered;
                }
            }
            else
            {
                SCTrace(L"CreateSample failed 0x%08X", hr);
            }
        }
    }

    m_reader.ReportConsumer(false, m_format, m_width, m_height, m_fpsNum, m_fpsDen, m_framesDelivered);
    m_reader.Close();
    if (SUCCEEDED(hrCo))
        CoUninitialize();
}
