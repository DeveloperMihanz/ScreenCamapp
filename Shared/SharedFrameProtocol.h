// ScreenCam Virtual Camera - shared-memory frame transport protocol.
//
// The desktop application (producer, interactive user session) and the media
// source (consumer, hosted by the Windows Camera Frame Server service in
// session 0 as LOCAL SERVICE) exchange frames through ONE named, pagefile-backed
// section in the Global\ namespace:
//
//   [ SCSharedHeader (4 KiB page) ][ slot 0 : NV12 frame ][ slot 1 : NV12 frame ]
//
// * The media source creates the section (services hold SeCreateGlobalPrivilege)
//   with a DACL that grants Interactive Users read/write. The desktop app opens it.
//   An elevated desktop app may also create it with the same DACL.
// * Two slots + per-slot sequence counters (a "seqlock"): the writer always fills
//   the slot that is NOT the latest one, so readers almost never collide, and a
//   reader that does collide detects it and keeps its previous frame.
// * No cross-session kernel events are needed: the consumer delivers frames on
//   its own clock at the negotiated frame rate and simply re-reads the latest
//   slot; the producer only writes when the screen content changed.
// * Heartbeats (GetTickCount64 milliseconds, system-wide) let each side detect
//   whether the other side is alive.
//
// This header is portable C++ (no Windows headers required) so the protocol
// logic can be unit tested on any platform.
#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>

#if defined(_WIN32)
#include <intrin.h>
#endif

namespace screencam {

constexpr uint32_t kSharedMagic        = 0x4D435353; // 'SSCM'
constexpr uint32_t kSharedVersion      = 1;
constexpr uint32_t kSlotCount          = 2;
constexpr uint32_t kHeaderBytes        = 4096;
constexpr uint32_t kMaxWidth           = 1920;
constexpr uint32_t kMaxHeight          = 1080;
constexpr uint32_t kPixelFormatNV12    = 1;

// NV12 = full-resolution Y plane + half-resolution interleaved UV plane.
constexpr uint32_t Nv12Size(uint32_t w, uint32_t h) { return w * h + w * (h / 2); }
constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

constexpr uint32_t kSlotCapacity  = AlignUp(Nv12Size(kMaxWidth, kMaxHeight), 4096);
constexpr uint32_t kSectionBytes  = kHeaderBytes + kSlotCapacity * kSlotCount;

// Name of the section. "Global\" makes it visible across sessions.
#define SCREENCAM_SHARED_SECTION_NAME L"Global\\ScreenCamVirtualCamera_Frames_v1"

// DACL for the section (SDDL):
//   SY = LocalSystem, BA = Administrators, LS = LOCAL SERVICE (Frame Server),
//   NS = NETWORK SERVICE, IU = Interactive users (desktop app). Everyone else: no access.
#define SCREENCAM_SHARED_SECTION_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;IU)"

#pragma pack(push, 8)
struct SCFrameSlot
{
    volatile int32_t sequence;   // odd while being written
    uint32_t width;
    uint32_t height;
    uint32_t format;             // kPixelFormatNV12
    uint32_t stride;             // bytes per row for Y and UV plane (== width)
    uint32_t dataSize;           // bytes of valid data
    int64_t  timestamp100ns;     // producer capture time (QPC based, 100 ns)
    uint32_t frameNumber;
    uint32_t reserved;
};

struct SCSharedHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t slotCount;
    uint32_t slotCapacity;
    uint32_t sectionSize;

    volatile int32_t latestSlot;        // -1 = no frame yet
    volatile int32_t frameCounter;      // incremented on every published frame

    // Producer (desktop app) state
    volatile int64_t producerHeartbeatMs;
    volatile int32_t producerActive;    // 1 while the app's virtual camera is started
    volatile int32_t producerFps;
    volatile int32_t preferredWidth;    // the resolution chosen in the app
    volatile int32_t preferredHeight;

    // Consumer (media source) state
    volatile int64_t consumerHeartbeatMs;
    volatile int32_t consumerWidth;
    volatile int32_t consumerHeight;
    volatile int32_t consumerFpsNum;
    volatile int32_t consumerFpsDen;
    volatile int32_t consumerFormat;    // 1 = NV12, 2 = RGB32
    volatile int32_t consumerStreaming; // 1 while a stream is running
    volatile int64_t consumerFrames;    // samples delivered to the Frame Server

    SCFrameSlot slots[kSlotCount];
};
#pragma pack(pop)

static_assert(sizeof(SCSharedHeader) <= kHeaderBytes, "header must fit in one page");

// ---------------------------------------------------------------------------
// Portable atomic helpers (full barriers).
// ---------------------------------------------------------------------------
#if defined(_WIN32)
inline int32_t AtomicLoad(volatile int32_t* p) { return (int32_t)_InterlockedCompareExchange((volatile long*)p, 0, 0); }
inline void    AtomicStore(volatile int32_t* p, int32_t v) { _InterlockedExchange((volatile long*)p, (long)v); }
inline int32_t AtomicIncrement(volatile int32_t* p) { return (int32_t)_InterlockedIncrement((volatile long*)p); }
inline int64_t AtomicLoad64(volatile int64_t* p) { return _InterlockedCompareExchange64((volatile long long*)p, 0, 0); }
inline void    AtomicStore64(volatile int64_t* p, int64_t v) { _InterlockedExchange64((volatile long long*)p, v); }
inline void    FullBarrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
#else
inline int32_t AtomicLoad(volatile int32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void    AtomicStore(volatile int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicIncrement(volatile int32_t* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline int64_t AtomicLoad64(volatile int64_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void    AtomicStore64(volatile int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
inline void    FullBarrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
#endif

inline uint8_t* SlotData(SCSharedHeader* h, uint32_t slot)
{
    return reinterpret_cast<uint8_t*>(h) + kHeaderBytes + (size_t)slot * kSlotCapacity;
}

// Initializes a freshly created (zero-filled) section.
inline void InitializeHeader(SCSharedHeader* h)
{
    std::memset(h, 0, sizeof(SCSharedHeader));
    h->headerSize   = kHeaderBytes;
    h->slotCount    = kSlotCount;
    h->slotCapacity = kSlotCapacity;
    h->sectionSize  = kSectionBytes;
    h->latestSlot   = -1;
    h->version      = kSharedVersion;
    FullBarrier();
    h->magic        = kSharedMagic; // written last
    FullBarrier();
}

inline bool IsHeaderValid(const SCSharedHeader* h)
{
    return h->magic == kSharedMagic && h->version == kSharedVersion &&
           h->slotCount == kSlotCount && h->slotCapacity == kSlotCapacity;
}

// ---------------------------------------------------------------------------
// Writer: publishes one NV12 frame. Planes may have any pitch; they are stored
// tightly packed (stride == width). Returns false on invalid arguments.
// ---------------------------------------------------------------------------
inline bool PublishNv12Frame(SCSharedHeader* h,
                             const uint8_t* y, uint32_t yPitch,
                             const uint8_t* uv, uint32_t uvPitch,
                             uint32_t width, uint32_t height,
                             int64_t timestamp100ns)
{
    if (!h || !y || !uv || width == 0 || height == 0 || (width & 1) || (height & 1) ||
        width > kMaxWidth || height > kMaxHeight)
        return false;

    const int32_t latest = AtomicLoad(&h->latestSlot);
    const uint32_t slot = (latest < 0) ? 0u : (uint32_t)((latest + 1) % (int32_t)kSlotCount);
    SCFrameSlot& s = h->slots[slot];

    AtomicIncrement(&s.sequence);               // -> odd: writing
    uint8_t* dst = SlotData(h, slot);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(dst + (size_t)row * width, y + (size_t)row * yPitch, width);
    uint8_t* dstUV = dst + (size_t)width * height;
    for (uint32_t row = 0; row < height / 2; ++row)
        std::memcpy(dstUV + (size_t)row * width, uv + (size_t)row * uvPitch, width);
    s.width = width;
    s.height = height;
    s.format = kPixelFormatNV12;
    s.stride = width;
    s.dataSize = Nv12Size(width, height);
    s.timestamp100ns = timestamp100ns;
    s.frameNumber = (uint32_t)AtomicLoad(&h->frameCounter) + 1;
    FullBarrier();
    AtomicIncrement(&s.sequence);               // -> even: complete
    AtomicStore(&h->latestSlot, (int32_t)slot);
    AtomicIncrement(&h->frameCounter);
    return true;
}

// ---------------------------------------------------------------------------
// Reader: copies the latest complete frame into 'dst' (capacity bytes).
// Returns true and fills width/height/frameCounter if a consistent frame was
// copied. Returns false if no frame exists, the frame was being overwritten,
// or the frame has not changed since 'lastFrameCounter'.
// ---------------------------------------------------------------------------
inline bool ReadLatestNv12Frame(SCSharedHeader* h, uint8_t* dst, uint32_t capacity,
                                int32_t lastFrameCounter,
                                uint32_t* width, uint32_t* height, int32_t* frameCounter)
{
    if (!h || !IsHeaderValid(h))
        return false;
    const int32_t counter = AtomicLoad(&h->frameCounter);
    if (counter == lastFrameCounter)
        return false;
    const int32_t latest = AtomicLoad(&h->latestSlot);
    if (latest < 0 || latest >= (int32_t)kSlotCount)
        return false;

    SCFrameSlot& s = h->slots[latest];
    const int32_t seq1 = AtomicLoad(&s.sequence);
    if (seq1 & 1)
        return false;
    const uint32_t w = s.width, hgt = s.height, size = s.dataSize;
    if (w == 0 || hgt == 0 || w > kMaxWidth || hgt > kMaxHeight ||
        size != Nv12Size(w, hgt) || size > capacity || s.format != kPixelFormatNV12)
        return false;
    std::memcpy(dst, SlotData(h, (uint32_t)latest), size);
    FullBarrier();
    const int32_t seq2 = AtomicLoad(&s.sequence);
    if (seq1 != seq2)
        return false; // torn read; caller keeps its previous frame

    *width = w;
    *height = hgt;
    *frameCounter = counter;
    return true;
}

} // namespace screencam
