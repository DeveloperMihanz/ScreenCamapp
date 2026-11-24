// ScreenCam Virtual Camera Source - reads frames published by the desktop app.
#pragma once

#include "Common.h"
#include "SharedFrameProtocol.h"

enum class OutputFormat : int32_t { NV12 = 1, RGB32 = 2 };

class FrameReader
{
public:
    FrameReader();
    ~FrameReader();

    // Creates (or opens) the shared section. Cheap to call repeatedly; it is
    // rate-limited internally.
    void EnsureOpen();
    void Close();

    // Renders the latest frame (or a "no signal" image when the app is not
    // producing) into a destination buffer of the negotiated format.
    // For NV12, 'dst' is the Y plane and the UV plane follows at dst + pitch * height.
    void Render(OutputFormat format, uint8_t* dst, LONG pitch, uint32_t width, uint32_t height);

    // Reports the negotiated format / delivery statistics to the desktop app.
    void ReportConsumer(bool streaming, OutputFormat format, uint32_t width, uint32_t height,
                        uint32_t fpsNum, uint32_t fpsDen, int64_t framesDelivered);

    // Preferred output mode selected in the desktop app (0 if unknown).
    bool GetPreferredMode(uint32_t* width, uint32_t* height, uint32_t* fps);

private:
    bool IsProducerAlive() const;

    HANDLE m_section = nullptr;
    screencam::SCSharedHeader* m_header = nullptr;
    ULONGLONG m_lastOpenAttempt = 0;

    std::vector<uint8_t> m_frame;    // last good NV12 frame from the producer
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    int32_t m_lastCounter = -1;
    uint32_t m_phase = 0;
};
