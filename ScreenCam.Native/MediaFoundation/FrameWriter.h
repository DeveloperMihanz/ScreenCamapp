// ScreenCam.Native - producer side of the shared-memory frame transport.
//
// The section is normally created by the media source inside the Frame Server
// when an application opens the camera; this class keeps trying to open it and,
// once open, keeps the handle so the section survives consumer restarts.
#pragma once

#include "NativeCommon.h"
#include "SharedFrameProtocol.h"

namespace screencam
{
    struct ConsumerInfo
    {
        bool present = false;    // media source alive (heartbeat fresh)
        bool streaming = false;  // ... and an app is streaming
        int32_t width = 0, height = 0, fpsNum = 0, fpsDen = 0, format = 0;
        int64_t frames = 0;
    };

    class FrameWriter
    {
    public:
        FrameWriter() = default;
        ~FrameWriter();
        FrameWriter(const FrameWriter&) = delete;
        FrameWriter& operator=(const FrameWriter&) = delete;

        // Tries to open (or, when elevated, create) the section. Rate limited.
        // Returns true if connected.
        bool TryConnect();
        bool IsConnected() const { return m_header != nullptr; }
        void Disconnect();

        void SetProducerState(bool active, int32_t fps, int32_t width, int32_t height);
        void Heartbeat();
        bool Publish(const uint8_t* y, uint32_t yPitch, const uint8_t* uv, uint32_t uvPitch,
                     uint32_t width, uint32_t height, int64_t timestamp100ns);
        ConsumerInfo GetConsumerInfo() const;

    private:
        HANDLE m_section = nullptr;
        SCSharedHeader* m_header = nullptr;
        ULONGLONG m_lastAttempt = 0;
    };
}
