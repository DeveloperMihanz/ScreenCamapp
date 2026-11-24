// ScreenCam.Native - screen region capture pipeline.
//
//   Windows Graphics Capture (monitor)  --FrameArrived-->  GPU crop copy (BGRA)
//        --worker @ fps-->  D3D11 video processor: scale + BGRA->NV12 (GPU)
//        --> staging map --> shared memory (camera)  +  BGRA preview (UI)
//
// If the GPU video processor is unavailable the worker falls back to a CPU
// scaler/converter. Frames are only processed when the screen content changed,
// so a static desktop costs almost nothing.
#pragma once

#include "NativeCommon.h"
#include "ScreenCamNative.h"
#include "FrameWriter.h"

#include <d3d11_4.h>
#include <vector>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace screencam
{
    class ScreenCapture
    {
    public:
        ScreenCapture();
        ~ScreenCapture();
        ScreenCapture(const ScreenCapture&) = delete;
        ScreenCapture& operator=(const ScreenCapture&) = delete;

        static bool IsSupported();

        int32_t Start(const SC_CaptureConfig& config);
        int32_t Update(const SC_CaptureConfig& config);
        void Stop();
        bool IsRunning() const { return m_running.load(); }

        void SetCameraActive(bool active);
        int32_t GetPreview(void* buffer, int32_t stride, int32_t capacity, int32_t* width, int32_t* height, int64_t* sequence);
        void FillStatus(SC_Status& status);

    private:
        struct Config
        {
            RECT region{};            // monitor-relative, physical pixels
            int32_t outW = 1280, outH = 720, fps = 30;
            int32_t showCursor = 1, scaleMode = 0, previewEnabled = 1, previewMaxWidth = 640, hideBorder = 1;
        };

        int32_t StartLocked(const SC_CaptureConfig& config);
        void StopLocked();
        static int32_t Normalize(const SC_CaptureConfig& in, HMONITOR* monitor, RECT* monitorRect, Config* out);
        bool CreateDevice();
        int32_t StartSession(HMONITOR monitor, const RECT& monitorRect, const Config& config);
        void StopSession();
        void ApplySessionOptions(const Config& config);

        void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender);
        void WorkerLoop();
        void ProcessFrame(const Config& config, bool publish, bool preview);
        bool ProcessGpu(const Config& config, D3D11_MAPPED_SUBRESOURCE* mapped);
        bool ProcessCpu(const Config& config);
        bool EnsureVideoProcessor(const Config& config);
        void ReleaseVideoProcessor();
        void EmitFrame(const Config& config, const uint8_t* y, uint32_t yPitch, const uint8_t* uv, uint32_t uvPitch,
                       bool publish, bool preview);

        // Threading
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_stopping{ false };
        std::atomic<bool> m_cameraActive{ false };
        std::atomic<bool> m_dirty{ false };
        std::atomic<bool> m_forceProcess{ false };
        std::atomic<bool> m_captureLost{ false };
        std::thread m_worker;
        HANDLE m_exitEvent = nullptr;
        HANDLE m_timer = nullptr;
        std::mutex m_sessionLock;       // Start/Stop/Update (UI thread)
        std::mutex m_configLock;
        Config m_config;
        HMONITOR m_monitor = nullptr;
        RECT m_monitorRect{};

        // Direct3D (all immediate-context use is serialized by m_gpuLock)
        std::mutex m_gpuLock;
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
        winrt::com_ptr<ID3D11VideoContext> m_videoContext;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
        std::atomic<bool> m_gpuPath{ true };

        // Capture objects
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
        winrt::event_token m_frameToken{};
        winrt::event_token m_closedToken{};
        winrt::Windows::Graphics::SizeInt32 m_poolSize{ 0, 0 };

        // Cropped capture (BGRA) - written in OnFrameArrived
        winrt::com_ptr<ID3D11Texture2D> m_cropTexture;
        int32_t m_cropW = 0, m_cropH = 0;

        // GPU video processor resources
        winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_vpEnum;
        winrt::com_ptr<ID3D11VideoProcessor> m_vp;
        winrt::com_ptr<ID3D11VideoProcessorInputView> m_vpInput;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> m_vpOutput;
        winrt::com_ptr<ID3D11Texture2D> m_nv12Texture;
        winrt::com_ptr<ID3D11Texture2D> m_nv12Staging;
        ID3D11Texture2D* m_vpInputSource = nullptr; // identity of the texture m_vpInput was created for
        int32_t m_vpCropW = 0, m_vpCropH = 0, m_vpOutW = 0, m_vpOutH = 0;

        // CPU fallback resources
        winrt::com_ptr<ID3D11Texture2D> m_bgraStaging;
        int32_t m_bgraStagingW = 0, m_bgraStagingH = 0;
        std::vector<uint8_t> m_cpuNv12;

        // Camera output (worker thread only)
        FrameWriter m_writer;
        std::mutex m_statusLock;
        ConsumerInfo m_consumer;
        bool m_sharedConnected = false;

        // Preview
        std::mutex m_previewLock;
        std::vector<uint8_t> m_preview;
        int32_t m_previewW = 0, m_previewH = 0;
        int64_t m_previewSeq = 0;

        // Statistics
        std::atomic<int64_t> m_framesCaptured{ 0 };
        std::atomic<int64_t> m_framesPublished{ 0 };
        std::atomic<double> m_captureFps{ 0.0 };
    };
}
