// ScreenCam.Native - screen region capture pipeline (Windows Graphics Capture + D3D11).
#include "ScreenCapture.h"
#include "FrameConvert.h"

#include <inspectable.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>

#include <algorithm>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace screencam
{
    namespace
    {
        // QueryPerformanceCounter in 100 ns units (same time base as MFGetSystemTime).
        int64_t Now100ns()
        {
            static const int64_t freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (int64_t)f.QuadPart; }();
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            const int64_t whole = c.QuadPart / freq;
            const int64_t part = c.QuadPart % freq;
            return whole * 10000000ll + part * 10000000ll / freq;
        }

        int32_t EvenClamp(int32_t v, int32_t lo, int32_t hi)
        {
            return std::clamp(v, lo, hi) & ~1;
        }

        bool PropertyPresent(const wchar_t* type, const wchar_t* property)
        {
            try
            {
                return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(type, property);
            }
            catch (...)
            {
                return false;
            }
        }
    }

    ScreenCapture::ScreenCapture()
    {
        m_exitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer)
            m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ScreenCapture::~ScreenCapture()
    {
        Stop();
        if (m_exitEvent) CloseHandle(m_exitEvent);
        if (m_timer) CloseHandle(m_timer);
    }

    bool ScreenCapture::IsSupported()
    {
        try
        {
            return wgc::GraphicsCaptureSession::IsSupported();
        }
        catch (...)
        {
            return false;
        }
    }

    // ------------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------------
    int32_t ScreenCapture::Normalize(const SC_CaptureConfig& in, HMONITOR* monitor, RECT* monitorRect, Config* out)
    {
        if (in.regionWidth < 2 || in.regionHeight < 2)
        {
            SetLastErrorMessage(L"The selected screen area is empty (%dx%d).", in.regionWidth, in.regionHeight);
            return SC_E_INVALIDARG;
        }
        RECT region{ in.regionLeft, in.regionTop, in.regionLeft + in.regionWidth, in.regionTop + in.regionHeight };

        // Capture works per monitor: use the monitor containing most of the region.
        HMONITOR mon = MonitorFromRect(&region, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!mon || !GetMonitorInfoW(mon, &mi))
        {
            SetLastErrorMessage(L"No display was found for the selected screen area.");
            return SC_E_NO_MONITOR;
        }
        RECT clipped{};
        if (!IntersectRect(&clipped, &region, &mi.rcMonitor))
        {
            SetLastErrorMessage(L"The selected screen area is outside every display. Please select the area again.");
            return SC_E_NO_MONITOR;
        }

        Config c;
        c.region = { clipped.left - mi.rcMonitor.left, clipped.top - mi.rcMonitor.top,
                     clipped.right - mi.rcMonitor.left, clipped.bottom - mi.rcMonitor.top };
        c.outW = EvenClamp(in.outputWidth, 160, (int32_t)kMaxWidth);
        c.outH = EvenClamp(in.outputHeight, 120, (int32_t)kMaxHeight);
        c.fps = std::clamp(in.fps, 1, 60);
        c.showCursor = in.showCursor ? 1 : 0;
        c.scaleMode = std::clamp(in.scaleMode, 0, 2);
        c.previewEnabled = in.previewEnabled ? 1 : 0;
        c.previewMaxWidth = std::clamp(in.previewMaxWidth, 64, 1920);
        c.hideBorder = in.hideCaptureBorder ? 1 : 0;

        *monitor = mon;
        *monitorRect = mi.rcMonitor;
        *out = c;
        return SC_OK;
    }

    // ------------------------------------------------------------------------
    // Direct3D device
    // ------------------------------------------------------------------------
    bool ScreenCapture::CreateDevice()
    {
        if (m_device)
            return true;

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        struct Attempt { D3D_DRIVER_TYPE type; UINT flags; };
        const Attempt attempts[] =
        {
            { D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT },
            { D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT },
            { D3D_DRIVER_TYPE_WARP,     D3D11_CREATE_DEVICE_BGRA_SUPPORT }, // software fallback (no GPU / RDP)
        };

        HRESULT hr = E_FAIL;
        for (const auto& a : attempts)
        {
            winrt::com_ptr<ID3D11Device> device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            hr = D3D11CreateDevice(nullptr, a.type, nullptr, a.flags, levels, _countof(levels), D3D11_SDK_VERSION,
                                   device.put(), nullptr, context.put());
            if (SUCCEEDED(hr))
            {
                m_device = device;
                m_context = context;
                Trace(L"D3D11 device created (driver type %d, flags 0x%X)", (int)a.type, a.flags);
                break;
            }
        }
        if (!m_device)
        {
            SetLastErrorMessage(L"Could not create a Direct3D 11 device: %s", DescribeHResult(hr).c_str());
            return false;
        }

        // The capture frame pool uses the device from its own threads.
        if (auto mt = m_device.try_as<ID3D11Multithread>())
            mt->SetMultithreadProtected(TRUE);

        m_videoDevice = m_device.try_as<ID3D11VideoDevice>();
        m_videoContext = m_context.try_as<ID3D11VideoContext>();
        m_gpuPath = (m_videoDevice && m_videoContext);

        try
        {
            auto dxgi = m_device.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
            m_winrtDevice = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
        }
        catch (winrt::hresult_error const& e)
        {
            SetLastErrorMessage(L"Could not create the WinRT Direct3D device: %s", e.message().c_str());
            m_device = nullptr;
            m_context = nullptr;
            m_videoDevice = nullptr;
            m_videoContext = nullptr;
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Start / Update / Stop
    // ------------------------------------------------------------------------
    int32_t ScreenCapture::Start(const SC_CaptureConfig& config)
    {
        std::lock_guard<std::mutex> lock(m_sessionLock);
        return StartLocked(config);
    }

    int32_t ScreenCapture::StartLocked(const SC_CaptureConfig& in)
    {
        StopLocked();

        if (!IsSupported())
        {
            SetLastErrorMessage(L"Windows Graphics Capture is not supported on this system (Windows 10 version 1903 or later is required).");
            return SC_E_CAPTURE_UNSUPPORTED;
        }

        HMONITOR monitor = nullptr;
        RECT monitorRect{};
        Config config;
        int32_t rc = Normalize(in, &monitor, &monitorRect, &config);
        if (rc != SC_OK)
            return rc;
        if (!CreateDevice())
            return SC_E_FAIL;

        {
            std::lock_guard<std::mutex> cfgLock(m_configLock);
            m_config = config;
        }
        m_monitor = monitor;
        m_monitorRect = monitorRect;
        m_captureLost = false;
        m_dirty = false;
        m_forceProcess = true;

        rc = StartSession(monitor, monitorRect, config);
        if (rc != SC_OK)
            return rc;

        ResetEvent(m_exitEvent);
        m_stopping = false;
        try
        {
            m_worker = std::thread(&ScreenCapture::WorkerLoop, this);
        }
        catch (...)
        {
            StopSession();
            SetLastErrorMessage(L"Could not start the capture thread.");
            return SC_E_FAIL;
        }
        m_running = true;
        Trace(L"Capture started: region %ld,%ld-%ld,%ld on monitor %p, output %dx%d @ %d fps",
              config.region.left, config.region.top, config.region.right, config.region.bottom,
              (void*)monitor, config.outW, config.outH, config.fps);
        return SC_OK;
    }

    int32_t ScreenCapture::Update(const SC_CaptureConfig& in)
    {
        std::lock_guard<std::mutex> lock(m_sessionLock);
        if (!m_running)
            return StartLocked(in);

        HMONITOR monitor = nullptr;
        RECT monitorRect{};
        Config config;
        const int32_t rc = Normalize(in, &monitor, &monitorRect, &config);
        if (rc != SC_OK)
            return rc;

        Config old;
        {
            std::lock_guard<std::mutex> cfgLock(m_configLock);
            old = m_config;
        }
        const bool regionChanged = !EqualRect(&old.region, &config.region);
        if (monitor != m_monitor || !EqualRect(&monitorRect, &m_monitorRect) || regionChanged || m_captureLost)
        {
            // A new region needs a fresh frame, and Windows Graphics Capture only
            // delivers frames when content changes, so restart the (cheap) session.
            return StartLocked(in);
        }

        {
            std::lock_guard<std::mutex> cfgLock(m_configLock);
            m_config = config;
        }
        ApplySessionOptions(config);
        m_forceProcess = true;
        return SC_OK;
    }

    void ScreenCapture::Stop()
    {
        std::lock_guard<std::mutex> lock(m_sessionLock);
        StopLocked();
    }

    void ScreenCapture::StopLocked()
    {
        if (!m_running && !m_worker.joinable() && !m_session)
            return;
        m_stopping = true;
        SetEvent(m_exitEvent);
        if (m_worker.joinable())
            m_worker.join();
        StopSession();
        m_writer.Disconnect();
        {
            std::lock_guard<std::mutex> status(m_statusLock);
            m_consumer = ConsumerInfo{};
            m_sharedConnected = false;
        }
        m_running = false;
        m_captureFps = 0.0;
        Trace(L"Capture stopped");
    }

    int32_t ScreenCapture::StartSession(HMONITOR monitor, const RECT& /*monitorRect*/, const Config& config)
    {
        try
        {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem item{ nullptr };
            winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)));

            m_item = item;
            m_poolSize = item.Size();
            m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_poolSize);
            m_frameToken = m_pool.FrameArrived([this](wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&)
            {
                OnFrameArrived(sender);
            });
            m_closedToken = m_item.Closed([this](wgc::GraphicsCaptureItem const&, winrt::Windows::Foundation::IInspectable const&)
            {
                m_captureLost = true;
                Trace(L"Capture item closed (display removed or changed)");
            });
            m_session = m_pool.CreateCaptureSession(m_item);
            ApplySessionOptions(config);
            m_session.StartCapture();
            return SC_OK;
        }
        catch (winrt::hresult_error const& e)
        {
            SetLastErrorMessage(L"Could not start screen capture: %s (0x%08X)", e.message().c_str(), (unsigned)e.code());
            StopSession();
            return SC_E_CAPTURE_UNSUPPORTED;
        }
        catch (...)
        {
            SetLastErrorMessage(L"Could not start screen capture (unexpected error).");
            StopSession();
            return SC_E_FAIL;
        }
    }

    void ScreenCapture::ApplySessionOptions(const Config& config)
    {
        if (!m_session)
            return;
        static const bool hasCursor = PropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
        static const bool hasBorder = PropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
        try
        {
            if (hasCursor)
                m_session.IsCursorCaptureEnabled(config.showCursor != 0);
        }
        catch (...) {}
        try
        {
            // Windows 11: lets the app ask for no yellow capture border. Windows may
            // still show it depending on policy; failure is harmless.
            if (hasBorder)
                m_session.IsBorderRequired(config.hideBorder == 0);
        }
        catch (...) {}
    }

    void ScreenCapture::StopSession()
    {
        try { if (m_pool && m_frameToken.value) m_pool.FrameArrived(m_frameToken); } catch (...) {}
        try { if (m_item && m_closedToken.value) m_item.Closed(m_closedToken); } catch (...) {}
        try { if (m_session) m_session.Close(); } catch (...) {}
        try { if (m_pool) m_pool.Close(); } catch (...) {}
        m_frameToken = {};
        m_closedToken = {};
        m_session = nullptr;
        m_pool = nullptr;
        m_item = nullptr;

        std::lock_guard<std::mutex> gpu(m_gpuLock);
        m_cropTexture = nullptr;
        m_cropW = m_cropH = 0;
        ReleaseVideoProcessor();
        m_bgraStaging = nullptr;
        m_bgraStagingW = m_bgraStagingH = 0;
    }

    void ScreenCapture::SetCameraActive(bool active)
    {
        m_cameraActive = active;
        m_forceProcess = true;
    }

    // ------------------------------------------------------------------------
    // Capture callback (thread pool thread)
    // ------------------------------------------------------------------------
    void ScreenCapture::OnFrameArrived(wgc::Direct3D11CaptureFramePool const& sender)
    {
        try
        {
            auto frame = sender.TryGetNextFrame();
            if (!frame)
                return;
            if (m_stopping)
            {
                frame.Close();
                return;
            }

            const auto contentSize = frame.ContentSize();
            RECT region;
            {
                std::lock_guard<std::mutex> cfgLock(m_configLock);
                region = m_config.region;
            }

            {
                std::lock_guard<std::mutex> gpu(m_gpuLock);
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> texture;
                winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
                D3D11_TEXTURE2D_DESC td{};
                texture->GetDesc(&td);

                const LONG maxW = std::min<LONG>((LONG)td.Width, contentSize.Width);
                const LONG maxH = std::min<LONG>((LONG)td.Height, contentSize.Height);
                const LONG left = std::clamp<LONG>(region.left, 0, maxW);
                const LONG top = std::clamp<LONG>(region.top, 0, maxH);
                const LONG right = std::clamp<LONG>(region.right, 0, maxW);
                const LONG bottom = std::clamp<LONG>(region.bottom, 0, maxH);
                const int32_t w = (int32_t)(right - left) & ~1;
                const int32_t h = (int32_t)(bottom - top) & ~1;

                if (w >= 2 && h >= 2)
                {
                    if (!m_cropTexture || m_cropW != w || m_cropH != h)
                    {
                        D3D11_TEXTURE2D_DESC desc{};
                        desc.Width = (UINT)w;
                        desc.Height = (UINT)h;
                        desc.MipLevels = 1;
                        desc.ArraySize = 1;
                        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        desc.SampleDesc.Count = 1;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        m_cropTexture = nullptr;
                        winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, m_cropTexture.put()));
                        m_cropW = w;
                        m_cropH = h;
                        m_vpInput = nullptr;
                        m_vpInputSource = nullptr;
                    }
                    const D3D11_BOX box{ (UINT)left, (UINT)top, 0, (UINT)(left + w), (UINT)(top + h), 1 };
                    m_context->CopySubresourceRegion(m_cropTexture.get(), 0, 0, 0, 0, texture.get(), 0, &box);
                    m_dirty = true;
                }
            }
            frame.Close();
            ++m_framesCaptured;

            // Monitor resolution changed: resize the frame pool.
            if (contentSize.Width != m_poolSize.Width || contentSize.Height != m_poolSize.Height)
            {
                m_poolSize = contentSize;
                sender.Recreate(m_winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
            }
        }
        catch (winrt::hresult_error const& e)
        {
            Trace(L"FrameArrived error 0x%08X: %s", (unsigned)e.code(), e.message().c_str());
        }
        catch (...)
        {
            Trace(L"FrameArrived: unexpected exception");
        }
    }

    // ------------------------------------------------------------------------
    // Worker: paces processing at the output frame rate
    // ------------------------------------------------------------------------
    void ScreenCapture::WorkerLoop()
    {
        int64_t next = Now100ns();
        ULONGLONG lastPreview = 0, lastState = 0, lastFpsTick = GetTickCount64();
        int64_t lastCaptured = m_framesCaptured.load();
        bool needPublish = true, needPreview = true;
        int32_t lastOutW = 0, lastOutH = 0, lastScale = -1;

        for (;;)
        {
            Config cfg;
            {
                std::lock_guard<std::mutex> cfgLock(m_configLock);
                cfg = m_config;
            }
            const int64_t period = 10000000ll / std::max(1, cfg.fps);

            int64_t now = Now100ns();
            if (next > now)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(next - now);
                SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE waits[2] = { m_exitEvent, m_timer };
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
                    break;
            }
            else if (WaitForSingleObject(m_exitEvent, 0) == WAIT_OBJECT_0)
            {
                break;
            }
            now = Now100ns();
            next = (now - next > period) ? now + period : next + period;

            const ULONGLONG tick = GetTickCount64();
            const bool cameraActive = m_cameraActive.load();

            // Camera connection management
            if (cameraActive)
            {
                if (!m_writer.IsConnected() && m_writer.TryConnect())
                {
                    needPublish = true;
                    lastState = 0;
                }
                if (m_writer.IsConnected() && tick - lastState >= 250)
                {
                    m_writer.SetProducerState(true, cfg.fps, cfg.outW, cfg.outH);
                    lastState = tick;
                }
            }
            else if (m_writer.IsConnected())
            {
                m_writer.SetProducerState(false, cfg.fps, cfg.outW, cfg.outH);
                m_writer.Disconnect();
            }
            const ConsumerInfo consumer = m_writer.GetConsumerInfo();
            {
                std::lock_guard<std::mutex> status(m_statusLock);
                m_consumer = consumer;
                m_sharedConnected = m_writer.IsConnected();
            }

            // What needs to be (re)rendered?
            if (m_dirty.exchange(false) | m_forceProcess.exchange(false))
                needPublish = needPreview = true;
            if (cfg.outW != lastOutW || cfg.outH != lastOutH || cfg.scaleMode != lastScale)
            {
                needPublish = needPreview = true;
                lastOutW = cfg.outW;
                lastOutH = cfg.outH;
                lastScale = cfg.scaleMode;
            }

            const bool doPublish = needPublish && cameraActive && m_writer.IsConnected() && consumer.present;
            const bool doPreview = needPreview && cfg.previewEnabled && tick - lastPreview >= 66; // ~15 fps preview
            if (doPublish || doPreview)
            {
                ProcessFrame(cfg, doPublish, doPreview);
                if (doPublish)
                    needPublish = false;
                if (doPreview)
                {
                    needPreview = false;
                    lastPreview = tick;
                }
            }

            if (tick - lastFpsTick >= 1000)
            {
                const int64_t captured = m_framesCaptured.load();
                m_captureFps = (double)(captured - lastCaptured) * 1000.0 / (double)(tick - lastFpsTick);
                lastCaptured = captured;
                lastFpsTick = tick;
            }
        }

        if (m_writer.IsConnected())
        {
            Config cfg;
            {
                std::lock_guard<std::mutex> cfgLock(m_configLock);
                cfg = m_config;
            }
            m_writer.SetProducerState(false, cfg.fps, cfg.outW, cfg.outH);
        }
    }

    void ScreenCapture::ProcessFrame(const Config& cfg, bool publish, bool preview)
    {
        std::lock_guard<std::mutex> gpu(m_gpuLock);
        if (!m_cropTexture)
            return;

        if (m_gpuPath)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (ProcessGpu(cfg, &mapped))
            {
                const uint8_t* y = static_cast<const uint8_t*>(mapped.pData);
                const uint8_t* uv = y + (size_t)mapped.RowPitch * cfg.outH;
                EmitFrame(cfg, y, mapped.RowPitch, uv, mapped.RowPitch, publish, preview);
                m_context->Unmap(m_nv12Staging.get(), 0);
                return;
            }
            Trace(L"GPU video processing unavailable - switching to CPU conversion");
            m_gpuPath = false;
            ReleaseVideoProcessor();
        }

        if (ProcessCpu(cfg))
        {
            const uint8_t* y = m_cpuNv12.data();
            const uint8_t* uv = y + (size_t)cfg.outW * cfg.outH;
            EmitFrame(cfg, y, (uint32_t)cfg.outW, uv, (uint32_t)cfg.outW, publish, preview);
        }
    }

    bool ScreenCapture::EnsureVideoProcessor(const Config& cfg)
    {
        if (!m_videoDevice || !m_videoContext)
            return false;
        if (m_vp && m_vpCropW == m_cropW && m_vpCropH == m_cropH && m_vpOutW == cfg.outW && m_vpOutH == cfg.outH)
            return true;

        ReleaseVideoProcessor();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = { (UINT)cfg.fps, 1 };
        desc.InputWidth = (UINT)m_cropW;
        desc.InputHeight = (UINT)m_cropH;
        desc.OutputFrameRate = { (UINT)cfg.fps, 1 };
        desc.OutputWidth = (UINT)cfg.outW;
        desc.OutputHeight = (UINT)cfg.outH;
        desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&desc, m_vpEnum.put())))
            return false;

        UINT support = 0;
        if (FAILED(m_vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
            return false;
        support = 0;
        if (FAILED(m_vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            return false;
        if (FAILED(m_videoDevice->CreateVideoProcessor(m_vpEnum.get(), 0, m_vp.put())))
            return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)cfg.outW;
        td.Height = (UINT)cfg.outH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_nv12Texture.put())))
            return false;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_nv12Staging.put())))
            return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        if (FAILED(m_videoDevice->CreateVideoProcessorOutputView(m_nv12Texture.get(), m_vpEnum.get(), &ovd, m_vpOutput.put())))
            return false;

        // Static processor state
        ID3D11VideoProcessor* vp = m_vp.get();
        m_videoContext->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoContext->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
        inSpace.Usage = 0;          // playback
        inSpace.RGB_Range = 0;      // full range 0-255 (desktop)
        m_videoContext->VideoProcessorSetStreamColorSpace(vp, 0, &inSpace);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
        outSpace.Usage = 0;
        outSpace.YCbCr_Matrix = 1;  // BT.709
        outSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetOutputColorSpace(vp, &outSpace);
        const RECT full{ 0, 0, cfg.outW, cfg.outH };
        m_videoContext->VideoProcessorSetOutputTargetRect(vp, TRUE, &full);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.R = 0.0f;
        black.RGBA.G = 0.0f;
        black.RGBA.B = 0.0f;
        black.RGBA.A = 1.0f;
        m_videoContext->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);

        m_vpCropW = m_cropW;
        m_vpCropH = m_cropH;
        m_vpOutW = cfg.outW;
        m_vpOutH = cfg.outH;
        return true;
    }

    void ScreenCapture::ReleaseVideoProcessor()
    {
        m_vpInput = nullptr;
        m_vpInputSource = nullptr;
        m_vpOutput = nullptr;
        m_nv12Staging = nullptr;
        m_nv12Texture = nullptr;
        m_vp = nullptr;
        m_vpEnum = nullptr;
        m_vpCropW = m_vpCropH = m_vpOutW = m_vpOutH = 0;
    }

    bool ScreenCapture::ProcessGpu(const Config& cfg, D3D11_MAPPED_SUBRESOURCE* mapped)
    {
        if (!EnsureVideoProcessor(cfg))
            return false;

        if (!m_vpInput || m_vpInputSource != m_cropTexture.get())
        {
            m_vpInput = nullptr;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.FourCC = 0;
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivd.Texture2D.MipSlice = 0;
            ivd.Texture2D.ArraySlice = 0;
            if (FAILED(m_videoDevice->CreateVideoProcessorInputView(m_cropTexture.get(), m_vpEnum.get(), &ivd, m_vpInput.put())))
                return false;
            m_vpInputSource = m_cropTexture.get();
        }

        Rect32 src{ 0, 0, m_cropW, m_cropH };
        Rect32 dst{ 0, 0, cfg.outW, cfg.outH };
        switch ((ScaleMode)cfg.scaleMode)
        {
        case ScaleMode::Fit:     dst = ComputeFitRect(m_cropW, m_cropH, cfg.outW, cfg.outH); break;
        case ScaleMode::Fill:    src = ComputeFillSourceRect(m_cropW, m_cropH, cfg.outW, cfg.outH); break;
        case ScaleMode::Stretch: break;
        }
        const RECT srcRect{ src.x, src.y, src.x + src.w, src.y + src.h };
        const RECT dstRect{ dst.x, dst.y, dst.x + dst.w, dst.y + dst.h };
        m_videoContext->VideoProcessorSetStreamSourceRect(m_vp.get(), 0, TRUE, &srcRect);
        m_videoContext->VideoProcessorSetStreamDestRect(m_vp.get(), 0, TRUE, &dstRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = m_vpInput.get();
        if (FAILED(m_videoContext->VideoProcessorBlt(m_vp.get(), m_vpOutput.get(), 0, 1, &stream)))
            return false;

        m_context->CopyResource(m_nv12Staging.get(), m_nv12Texture.get());
        return SUCCEEDED(m_context->Map(m_nv12Staging.get(), 0, D3D11_MAP_READ, 0, mapped));
    }

    bool ScreenCapture::ProcessCpu(const Config& cfg)
    {
        if (!m_bgraStaging || m_bgraStagingW != m_cropW || m_bgraStagingH != m_cropH)
        {
            m_bgraStaging = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)m_cropW;
            td.Height = (UINT)m_cropH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_bgraStaging.put())))
                return false;
            m_bgraStagingW = m_cropW;
            m_bgraStagingH = m_cropH;
        }

        m_context->CopyResource(m_bgraStaging.get(), m_cropTexture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_bgraStaging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;

        m_cpuNv12.resize(Nv12Size((uint32_t)cfg.outW, (uint32_t)cfg.outH));
        uint8_t* y = m_cpuNv12.data();
        uint8_t* uv = y + (size_t)cfg.outW * cfg.outH;
        Rect32 src{ 0, 0, m_cropW, m_cropH };
        Rect32 dst{ 0, 0, cfg.outW, cfg.outH };
        switch ((ScaleMode)cfg.scaleMode)
        {
        case ScaleMode::Fit:     dst = ComputeFitRect(m_cropW, m_cropH, cfg.outW, cfg.outH); break;
        case ScaleMode::Fill:    src = ComputeFillSourceRect(m_cropW, m_cropH, cfg.outW, cfg.outH); break;
        case ScaleMode::Stretch: break;
        }
        if (dst.w != cfg.outW || dst.h != cfg.outH)
            FillNv12Black(y, cfg.outW, uv, cfg.outW, cfg.outW, cfg.outH);
        BgraToNv12IntoRect(static_cast<const uint8_t*>(mapped.pData), (int32_t)mapped.RowPitch, src,
                           y, cfg.outW, uv, cfg.outW, dst);
        m_context->Unmap(m_bgraStaging.get(), 0);
        return true;
    }

    void ScreenCapture::EmitFrame(const Config& cfg, const uint8_t* y, uint32_t yPitch, const uint8_t* uv, uint32_t uvPitch,
                                  bool publish, bool preview)
    {
        if (publish && m_writer.Publish(y, yPitch, uv, uvPitch, (uint32_t)cfg.outW, (uint32_t)cfg.outH, Now100ns()))
            ++m_framesPublished;

        if (preview)
        {
            int32_t pw = std::min(cfg.previewMaxWidth, cfg.outW) & ~1;
            int32_t ph = (int32_t)((int64_t)pw * cfg.outH / cfg.outW) & ~1;
            pw = std::max(2, pw);
            ph = std::max(2, ph);
            std::lock_guard<std::mutex> lock(m_previewLock);
            m_preview.resize((size_t)pw * ph * 4);
            Nv12ToBgraIntoRect(y, (int32_t)yPitch, uv, (int32_t)uvPitch, cfg.outW, cfg.outH,
                               m_preview.data(), pw * 4, Rect32{ 0, 0, pw, ph });
            m_previewW = pw;
            m_previewH = ph;
            ++m_previewSeq;
        }
    }

    // ------------------------------------------------------------------------
    // UI accessors
    // ------------------------------------------------------------------------
    int32_t ScreenCapture::GetPreview(void* buffer, int32_t stride, int32_t capacity, int32_t* width, int32_t* height, int64_t* sequence)
    {
        if (!width || !height || !sequence)
            return SC_E_INVALIDARG;
        std::lock_guard<std::mutex> lock(m_previewLock);
        *width = m_previewW;
        *height = m_previewH;
        if (m_previewW == 0 || m_previewSeq == *sequence)
            return 0;
        if (!buffer || stride < m_previewW * 4 || (int64_t)capacity < (int64_t)stride * m_previewH)
            return SC_E_BUFFER_TOO_SMALL;
        const uint8_t* src = m_preview.data();
        uint8_t* dst = static_cast<uint8_t*>(buffer);
        for (int32_t row = 0; row < m_previewH; ++row)
            memcpy(dst + (size_t)row * stride, src + (size_t)row * m_previewW * 4, (size_t)m_previewW * 4);
        *sequence = m_previewSeq;
        return 1;
    }

    void ScreenCapture::FillStatus(SC_Status& s)
    {
        s.captureRunning = m_running ? 1 : 0;
        s.gpuProcessing = m_gpuPath ? 1 : 0;
        s.framesCaptured = m_framesCaptured.load();
        s.framesPublished = m_framesPublished.load();
        s.captureFps = m_captureFps.load();
        s.captureLost = m_captureLost ? 1 : 0;
        std::lock_guard<std::mutex> lock(m_statusLock);
        s.sharedConnected = m_sharedConnected ? 1 : 0;
        s.consumerStreaming = m_consumer.streaming ? 1 : 0;
        s.consumerWidth = m_consumer.width;
        s.consumerHeight = m_consumer.height;
        s.consumerFpsNum = m_consumer.fpsNum;
        s.consumerFpsDen = m_consumer.fpsDen;
        s.consumerFormat = m_consumer.format;
        s.consumerFrames = m_consumer.frames;
    }
}
