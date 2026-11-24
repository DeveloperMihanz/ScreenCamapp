// ScreenCam.Native - shared-memory frame producer.
#include "FrameWriter.h"
#include <sddl.h>

namespace screencam
{
    FrameWriter::~FrameWriter()
    {
        Disconnect();
    }

    bool FrameWriter::TryConnect()
    {
        if (m_header)
            return true;
        const ULONGLONG now = GetTickCount64();
        if (m_lastAttempt != 0 && now - m_lastAttempt < 500)
            return false;
        m_lastAttempt = now;

        bool created = false;
        HANDLE section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SCREENCAM_SHARED_SECTION_NAME);
        if (!section)
        {
            // Only succeeds when the process holds SeCreateGlobalPrivilege
            // (e.g. the app runs elevated). Normally the media source creates it.
            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(SCREENCAM_SHARED_SECTION_SDDL, SDDL_REVISION_1, &sd, nullptr))
                sa.lpSecurityDescriptor = sd;
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0,
                                         kSectionBytes, SCREENCAM_SHARED_SECTION_NAME);
            created = section != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
            if (sd)
                LocalFree(sd);
            if (!section)
                return false;
        }

        void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSectionBytes);
        if (!view)
        {
            Trace(L"MapViewOfFile failed: %u", GetLastError());
            CloseHandle(section);
            return false;
        }
        m_section = section;
        m_header = static_cast<SCSharedHeader*>(view);
        if (created)
            InitializeHeader(m_header);
        Trace(L"Connected to camera shared memory (%s)", created ? L"created" : L"opened");
        return true;
    }

    void FrameWriter::Disconnect()
    {
        if (m_header)
        {
            if (IsHeaderValid(m_header))
                AtomicStore(&m_header->producerActive, 0);
            UnmapViewOfFile(m_header);
            m_header = nullptr;
        }
        if (m_section)
        {
            CloseHandle(m_section);
            m_section = nullptr;
        }
        m_lastAttempt = 0;
    }

    void FrameWriter::SetProducerState(bool active, int32_t fps, int32_t width, int32_t height)
    {
        if (!m_header || !IsHeaderValid(m_header))
            return;
        AtomicStore(&m_header->producerFps, fps);
        AtomicStore(&m_header->preferredWidth, width);
        AtomicStore(&m_header->preferredHeight, height);
        AtomicStore64(&m_header->producerHeartbeatMs, (int64_t)GetTickCount64());
        AtomicStore(&m_header->producerActive, active ? 1 : 0);
    }

    void FrameWriter::Heartbeat()
    {
        if (m_header && IsHeaderValid(m_header))
            AtomicStore64(&m_header->producerHeartbeatMs, (int64_t)GetTickCount64());
    }

    bool FrameWriter::Publish(const uint8_t* y, uint32_t yPitch, const uint8_t* uv, uint32_t uvPitch,
                              uint32_t width, uint32_t height, int64_t timestamp100ns)
    {
        if (!m_header || !IsHeaderValid(m_header))
            return false;
        return PublishNv12Frame(m_header, y, yPitch, uv, uvPitch, width, height, timestamp100ns);
    }

    ConsumerInfo FrameWriter::GetConsumerInfo() const
    {
        ConsumerInfo info;
        if (!m_header || !IsHeaderValid(m_header))
            return info;
        const int64_t hb = AtomicLoad64(&m_header->consumerHeartbeatMs);
        const bool fresh = hb != 0 && (int64_t)GetTickCount64() - hb < 2000;
        info.present = fresh;
        info.streaming = fresh && AtomicLoad(&m_header->consumerStreaming) == 1;
        info.width = AtomicLoad(&m_header->consumerWidth);
        info.height = AtomicLoad(&m_header->consumerHeight);
        info.fpsNum = AtomicLoad(&m_header->consumerFpsNum);
        info.fpsDen = AtomicLoad(&m_header->consumerFpsDen);
        info.format = AtomicLoad(&m_header->consumerFormat);
        info.frames = AtomicLoad64(&m_header->consumerFrames);
        return info;
    }
}
