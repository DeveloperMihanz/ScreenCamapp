// ScreenCam Virtual Camera Source - the Media Foundation media source.
//
// Implements the interfaces the Windows Camera Frame Server expects from a
// custom (virtual camera) media source:
//   IMFMediaSourceEx, IMFGetService, IKsControl, IMFSampleAllocatorControl.
#pragma once

#include "Common.h"

class MediaStream;

class MediaSource final :
    public IMFMediaSourceEx,
    public IMFGetService,
    public ISCKsControl,
    public IMFSampleAllocatorControl,
    private RefCounted
{
public:
    static HRESULT CreateInstance(IMFAttributes* activationAttributes, REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InternalAddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return InternalRelease(); }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage) override;

private:
    MediaSource() = default;
    ~MediaSource() override;
    HRESULT Initialize(IMFAttributes* activationAttributes);
    HRESULT CheckShutdown() const { return m_isShutdown ? MF_E_SHUTDOWN : S_OK; }

    static constexpr DWORD kStreamId = 0;

    CritSec m_lock;
    bool m_isShutdown = false;
    bool m_streamActivated = false;
    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFPresentationDescriptor> m_presentationDescriptor;
    ComPtr<MediaStream> m_stream;
};
