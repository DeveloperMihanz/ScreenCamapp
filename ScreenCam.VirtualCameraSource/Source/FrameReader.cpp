// ScreenCam Virtual Camera Source - shared memory frame consumer.
#include "FrameReader.h"
#include "FrameConvert.h"

using namespace screencam;

FrameReader::FrameReader()
{
    m_frame.resize(kSlotCapacity);
}

FrameReader::~FrameReader()
{
    Close();
}

void FrameReader::Close()
{
    if (m_header)
    {
        AtomicStore(&m_header->consumerStreaming, 0);
        UnmapViewOfFile(m_header);
        m_header = nullptr;
    }
    if (m_section)
    {
        CloseHandle(m_section);
        m_section = nullptr;
    }
}

void FrameReader::EnsureOpen()
{
    if (m_header)
        return;
    const ULONGLONG now = GetTickCount64();
    if (m_lastOpenAttempt != 0 && now - m_lastOpenAttempt < 1000)
        return;
    m_lastOpenAttempt = now;

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(SCREENCAM_SHARED_SECTION_SDDL, SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;

    // The Frame Server runs as LOCAL SERVICE, which holds SeCreateGlobalPrivilege,
    // so creating a Global\ section normally succeeds here.
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : nullptr,
                                        PAGE_READWRITE, 0, kSectionBytes, SCREENCAM_SHARED_SECTION_NAME);
    DWORD createError = GetLastError();
    bool created = section != nullptr && createError != ERROR_ALREADY_EXISTS;
    if (sd)
        LocalFree(sd);

    if (!section)
    {
        // Fall back to opening a section created by an elevated desktop app.
        section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SCREENCAM_SHARED_SECTION_NAME);
        if (!section)
        {
            SCTrace(L"Shared section unavailable (create error %u, open error %u)", createError, GetLastError());
            return;
        }
        created = false;
    }

    void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSectionBytes);
    if (!view)
    {
        SCTrace(L"MapViewOfFile failed %u", GetLastError());
        CloseHandle(section);
        return;
    }

    m_section = section;
    m_header = static_cast<SCSharedHeader*>(view);
    if (created)
        InitializeHeader(m_header);
    SCTrace(L"Shared section %s", created ? L"created" : L"opened");
}

bool FrameReader::IsProducerAlive() const
{
    if (!m_header || !IsHeaderValid(m_header))
        return false;
    if (AtomicLoad(&m_header->producerActive) != 1)
        return false;
    const int64_t hb = AtomicLoad64(&m_header->producerHeartbeatMs);
    const int64_t now = (int64_t)GetTickCount64();
    return hb != 0 && now - hb < 3000;
}

bool FrameReader::GetPreferredMode(uint32_t* width, uint32_t* height, uint32_t* fps)
{
    EnsureOpen();
    if (!m_header || !IsHeaderValid(m_header))
        return false;
    const int32_t w = AtomicLoad(&m_header->preferredWidth);
    const int32_t h = AtomicLoad(&m_header->preferredHeight);
    const int32_t f = AtomicLoad(&m_header->producerFps);
    if (w <= 0 || h <= 0)
        return false;
    *width = (uint32_t)w;
    *height = (uint32_t)h;
    *fps = f > 0 ? (uint32_t)f : 30;
    return true;
}

void FrameReader::ReportConsumer(bool streaming, OutputFormat format, uint32_t width, uint32_t height,
                                 uint32_t fpsNum, uint32_t fpsDen, int64_t framesDelivered)
{
    if (!m_header || !IsHeaderValid(m_header))
        return;
    AtomicStore(&m_header->consumerWidth, (int32_t)width);
    AtomicStore(&m_header->consumerHeight, (int32_t)height);
    AtomicStore(&m_header->consumerFpsNum, (int32_t)fpsNum);
    AtomicStore(&m_header->consumerFpsDen, (int32_t)fpsDen);
    AtomicStore(&m_header->consumerFormat, (int32_t)format);
    AtomicStore(&m_header->consumerStreaming, streaming ? 1 : 0);
    AtomicStore64(&m_header->consumerFrames, framesDelivered);
    AtomicStore64(&m_header->consumerHeartbeatMs, (int64_t)GetTickCount64());
}

void FrameReader::Render(OutputFormat format, uint8_t* dst, LONG pitch, uint32_t width, uint32_t height)
{
    EnsureOpen();

    const bool alive = IsProducerAlive();
    if (alive)
    {
        uint32_t w = 0, h = 0;
        int32_t counter = 0;
        if (ReadLatestNv12Frame(m_header, m_frame.data(), (uint32_t)m_frame.size(), m_lastCounter, &w, &h, &counter))
        {
            m_frameWidth = w;
            m_frameHeight = h;
            m_lastCounter = counter;
        }
    }

    const int32_t W = (int32_t)width, H = (int32_t)height;
    const bool haveFrame = alive && m_frameWidth > 0 && m_frameHeight > 0;

    // Source image (NV12) to draw: the producer frame or a generated pattern.
    const uint8_t* srcY;
    const uint8_t* srcUV;
    int32_t sw, sh;
    std::vector<uint8_t> pattern;
    if (haveFrame)
    {
        sw = (int32_t)m_frameWidth;
        sh = (int32_t)m_frameHeight;
        srcY = m_frame.data();
        srcUV = m_frame.data() + (size_t)sw * sh;
    }
    else
    {
        sw = W;
        sh = H;
        if (format == OutputFormat::NV12)
        {
            FillNv12NoSignal(dst, pitch, dst + (size_t)pitch * H, pitch, W, H, m_phase++);
            return;
        }
        pattern.resize(Nv12Size(width, height));
        FillNv12NoSignal(pattern.data(), W, pattern.data() + (size_t)W * H, W, W, H, m_phase++);
        srcY = pattern.data();
        srcUV = pattern.data() + (size_t)W * H;
    }

    const Rect32 full{ 0, 0, W, H };
    const Rect32 fit = (sw == W && sh == H) ? full : ComputeFitRect(sw, sh, W, H);

    if (format == OutputFormat::NV12)
    {
        uint8_t* dstUV = dst + (size_t)pitch * H;
        if (fit.w != W || fit.h != H)
            FillNv12Black(dst, pitch, dstUV, pitch, W, H);
        ScaleNv12IntoRect(srcY, sw, srcUV, sw, sw, sh, dst, pitch, dstUV, pitch, fit);
    }
    else
    {
        if (fit.w != W || fit.h != H)
            FillBgra(dst, pitch, W, H, 0, 0, 0);
        Nv12ToBgraIntoRect(srcY, sw, srcUV, sw, sw, sh, dst, pitch, fit);
    }
}
