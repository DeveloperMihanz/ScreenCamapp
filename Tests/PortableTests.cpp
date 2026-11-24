// ScreenCam - portable unit tests for the platform-independent core:
//   * Shared/SharedFrameProtocol.h  (shared-memory seqlock frame transport)
//   * Shared/FrameConvert.h         (BGRA <-> NV12 conversion and scaling)
//
// These run on any OS with a C++17 compiler:
//   Linux/macOS:  g++ -std=c++17 -O2 -Wall -Wextra -pthread -I../Shared PortableTests.cpp -o tests && ./tests
//   Windows:      cl /std:c++17 /EHsc /O2 /W4 /I..\Shared PortableTests.cpp && PortableTests.exe
#include "SharedFrameProtocol.h"
#include "FrameConvert.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace screencam;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// --- Geometry -----------------------------------------------------------------
static void TestFitRect()
{
    Rect32 r = ComputeFitRect(1920, 1080, 1280, 720);   // same aspect
    CHECK(r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 720);
    r = ComputeFitRect(800, 800, 1280, 720);            // square -> pillarbox
    CHECK(r.h == 720 && r.w == 720 && r.x == 280 && r.y == 0);
    r = ComputeFitRect(3000, 1000, 1280, 720);          // very wide -> letterbox
    CHECK(r.w == 1280 && r.h == 426 && r.x == 0 && (r.y % 2) == 0 && r.y + r.h <= 720);
    Rect32 f = ComputeFillSourceRect(800, 800, 1280, 720);
    CHECK(f.w == 800 && f.h == 450 && f.y == 175);
}

// --- Color conversion -------------------------------------------------------------
static void TestColorRoundTrip()
{
    const int W = 64, H = 32;
    std::vector<uint8_t> bgra(W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            uint8_t* p = &bgra[(y * W + x) * 4];
            // 2x2 blocks of one color (chroma is subsampled 2x2)
            p[0] = (uint8_t)((x / 2) * 8);       // B
            p[1] = (uint8_t)((y / 2) * 16);      // G
            p[2] = (uint8_t)(255 - (x / 2) * 8); // R
            p[3] = 255;
        }
    std::vector<uint8_t> nv12(Nv12Size(W, H));
    BgraToNv12IntoRect(bgra.data(), W * 4, Rect32{ 0, 0, W, H }, nv12.data(), W, nv12.data() + W * H, W, Rect32{ 0, 0, W, H });

    // Y must stay in studio range
    for (int i = 0; i < W * H; ++i)
        CHECK(nv12[i] >= 16 && nv12[i] <= 235);

    std::vector<uint8_t> back(W * H * 4);
    Nv12ToBgraIntoRect(nv12.data(), W, nv12.data() + W * H, W, W, H, back.data(), W * 4, Rect32{ 0, 0, W, H });
    int maxErr = 0;
    for (int i = 0; i < W * H * 4; ++i)
        maxErr = std::max(maxErr, std::abs((int)bgra[i] - (int)back[i]));
    std::printf("  BGRA->NV12->BGRA max channel error: %d\n", maxErr);
    CHECK(maxErr <= 6);

    // Pure colors
    const uint8_t white[4] = { 255, 255, 255, 255 }, black[4] = { 0, 0, 0, 255 };
    uint8_t y2[4], uv2[2];
    for (const uint8_t* c : { white, black })
    {
        uint8_t img[16];
        for (int i = 0; i < 4; ++i) std::memcpy(img + i * 4, c, 4);
        BgraToNv12IntoRect(img, 8, Rect32{ 0, 0, 2, 2 }, y2, 2, uv2, 2, Rect32{ 0, 0, 2, 2 });
        CHECK(y2[0] == (c == white ? 235 : 16));
        CHECK(std::abs(uv2[0] - 128) <= 1 && std::abs(uv2[1] - 128) <= 1);
    }
}

// --- Scaling -------------------------------------------------------------------
static void TestScaling()
{
    const int SW = 8, SH = 4;
    std::vector<uint8_t> src(Nv12Size(SW, SH));
    for (int i = 0; i < SW * SH; ++i) src[i] = (uint8_t)(16 + i);
    for (int i = 0; i < SW * SH / 2; ++i) src[SW * SH + i] = (uint8_t)(100 + i);

    // identity copy into a larger pitch
    const int pitch = 16;
    std::vector<uint8_t> dst(pitch * SH * 3 / 2, 0);
    ScaleNv12IntoRect(src.data(), SW, src.data() + SW * SH, SW, SW, SH, dst.data(), pitch, dst.data() + pitch * SH, pitch, Rect32{ 0, 0, SW, SH });
    bool same = true;
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x)
            same &= dst[y * pitch + x] == src[y * SW + x];
    CHECK(same);

    // 2x upscale: every source pixel becomes a 2x2 block
    std::vector<uint8_t> up(Nv12Size(SW * 2, SH * 2));
    ScaleNv12IntoRect(src.data(), SW, src.data() + SW * SH, SW, SW, SH, up.data(), SW * 2, up.data() + SW * 2 * SH * 2, SW * 2, Rect32{ 0, 0, SW * 2, SH * 2 });
    CHECK(up[0] == src[0] && up[1] == src[0] && up[SW * 2] == src[0] && up[2] == src[1]);
    const uint8_t* upUV = up.data() + SW * 2 * SH * 2;
    CHECK(upUV[0] == src[SW * SH] && upUV[1] == src[SW * SH + 1] && upUV[2] == src[SW * SH] && upUV[4] == src[SW * SH + 2]);

    // Letterboxed render: black outside the fit rectangle
    const int DW = 16, DH = 16;
    std::vector<uint8_t> box(Nv12Size(DW, DH));
    FillNv12Black(box.data(), DW, box.data() + DW * DH, DW, DW, DH);
    Rect32 fit = ComputeFitRect(SW, SH, DW, DH);
    ScaleNv12IntoRect(src.data(), SW, src.data() + SW * SH, SW, SW, SH, box.data(), DW, box.data() + DW * DH, DW, fit);
    CHECK(fit.y > 0 && box[0] == 16 && box[(fit.y) * DW] == src[0]);
}

// --- Shared-memory protocol ---------------------------------------------------------
static std::unique_ptr<uint8_t[]> AllocSection()
{
    std::unique_ptr<uint8_t[]> mem(new uint8_t[kSectionBytes]());
    InitializeHeader(reinterpret_cast<SCSharedHeader*>(mem.get()));
    return mem;
}

static void TestProtocolBasic()
{
    auto mem = AllocSection();
    auto* h = reinterpret_cast<SCSharedHeader*>(mem.get());
    CHECK(IsHeaderValid(h));
    CHECK(kSectionBytes == 4096 + 2 * kSlotCapacity);

    std::vector<uint8_t> out(kSlotCapacity);
    uint32_t w = 0, hh = 0;
    int32_t counter = -1;
    CHECK(!ReadLatestNv12Frame(h, out.data(), (uint32_t)out.size(), -1, &w, &hh, &counter)); // no frame yet

    const uint32_t W = 64, H = 48, pitch = 80;
    std::vector<uint8_t> y(pitch * H), uv(pitch * H / 2);
    for (size_t i = 0; i < y.size(); ++i) y[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < uv.size(); ++i) uv[i] = (uint8_t)(i * 3);
    CHECK(PublishNv12Frame(h, y.data(), pitch, uv.data(), pitch, W, H, 123));
    CHECK(ReadLatestNv12Frame(h, out.data(), (uint32_t)out.size(), -1, &w, &hh, &counter));
    CHECK(w == W && hh == H && counter == 1);
    bool ok = true;
    for (uint32_t r = 0; r < H; ++r)
        ok &= std::memcmp(out.data() + r * W, y.data() + r * pitch, W) == 0;
    for (uint32_t r = 0; r < H / 2; ++r)
        ok &= std::memcmp(out.data() + W * H + r * W, uv.data() + r * pitch, W) == 0;
    CHECK(ok);
    // unchanged frame is not re-read
    CHECK(!ReadLatestNv12Frame(h, out.data(), (uint32_t)out.size(), counter, &w, &hh, &counter));

    // invalid input rejected
    CHECK(!PublishNv12Frame(h, y.data(), pitch, uv.data(), pitch, 63, 48, 0));       // odd width
    CHECK(!PublishNv12Frame(h, y.data(), pitch, uv.data(), pitch, 4096, 2160, 0));   // too large
    // corrupt header rejected by the reader
    h->magic = 0;
    CHECK(!ReadLatestNv12Frame(h, out.data(), (uint32_t)out.size(), -1, &w, &hh, &counter));
}

// Concurrent writer/reader: every frame the reader accepts must be internally
// consistent (all bytes equal to the frame's tag), proving torn reads are rejected.
static void TestProtocolConcurrent()
{
    auto mem = AllocSection();
    auto* h = reinterpret_cast<SCSharedHeader*>(mem.get());
    const uint32_t W = 640, H = 480;
    std::atomic<bool> stop{ false };
    std::atomic<int> published{ 0 };

    std::thread writer([&]()
    {
        std::vector<uint8_t> frame(W * H);
        for (int i = 0; i < 3000 && !stop; ++i)
        {
            std::memset(frame.data(), (uint8_t)(i & 0xFF), frame.size());
            PublishNv12Frame(h, frame.data(), W, frame.data(), W, W, H, i);
            ++published;
        }
        stop = true;
    });

    std::vector<uint8_t> out(kSlotCapacity);
    int accepted = 0, inconsistent = 0;
    int32_t last = -1;
    while (!stop)
    {
        uint32_t w, hh;
        int32_t counter;
        if (ReadLatestNv12Frame(h, out.data(), (uint32_t)out.size(), last, &w, &hh, &counter))
        {
            last = counter;
            ++accepted;
            const uint8_t tag = out[0];
            const size_t n = Nv12Size(w, hh);
            for (size_t i = 0; i < n; i += 997)
                if (out[i] != tag) { ++inconsistent; break; }
        }
    }
    writer.join();
    std::printf("  concurrent: %d frames published, %d accepted by reader, %d inconsistent\n",
                published.load(), accepted, inconsistent);
    CHECK(accepted > 0);
    CHECK(inconsistent == 0);
}

int main()
{
    std::printf("ScreenCam portable tests\n");
    TestFitRect();
    TestColorRoundTrip();
    TestScaling();
    TestProtocolBasic();
    TestProtocolConcurrent();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
