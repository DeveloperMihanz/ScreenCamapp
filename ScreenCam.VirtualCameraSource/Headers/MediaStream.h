// ScreenCam Virtual Camera Source - the single video stream of the media source.
#pragma once

#include "Common.h"
#include "FrameReader.h"

class MediaStream final : public IMFMediaStream2, private RefCounted
{
public:
    static HRESULT CreateInstance(IMFMediaSource* source, DWORD streamId, MediaStream** stream);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InternalAddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return InternalRelease(); }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

    // Called by MediaSource
    HRESULT Start(IMFMediaType* mediaType);
    HRESULT Stop();
    HRESULT Shutdown();
    HRESULT SetAllocator(IUnknown* allocator);
    HRESULT GetAttributes(IMFAttributes** attributes);

private:
    MediaStream() = default;
    ~MediaStream() override;
    HRESULT Initialize(IMFMediaSource* source, DWORD streamId);
    HRESULT BuildMediaTypes(std::vector<ComPtr<IMFMediaType>>& types);
    HRESULT ApplyMediaType(IMFMediaType* mediaType);
    HRESULT CreateSample(IMFSample** sample);
    HRESULT FillSample(IMFSample* sample);
    void WorkerLoop();
    void StopWorker();
    HRESULT CheckShutdown() const { return m_isShutdown ? MF_E_SHUTDOWN : S_OK; }

    CritSec m_lock;
    bool m_isShutdown = false;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    DWORD m_streamId = 0;

    ComPtr<IMFMediaSource> m_source;          // released on Shutdown (breaks the cycle)
    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFVideoSampleAllocatorEx> m_allocator;
    ComPtr<IMFMediaType> m_currentType;
    bool m_allocatorInitialized = false;

    // Negotiated format
    OutputFormat m_format = OutputFormat::NV12;
    UINT32 m_width = 0;
    UINT32 m_height = 0;
    UINT32 m_fpsNum = 30;
    UINT32 m_fpsDen = 1;
    LONGLONG m_frameDuration = 333333;

    // Pending RequestSample tokens (nullptr tokens are valid)
    std::deque<ComPtr<IUnknown>> m_tokens;

    // Frame delivery thread
    std::thread m_worker;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_exitEvent = nullptr;
    HANDLE m_timer = nullptr;
    LONGLONG m_nextDeadline = 0;
    int64_t m_framesDelivered = 0;

    FrameReader m_reader;     // used only on the worker thread
};
