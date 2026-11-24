// ScreenCam Virtual Camera - CPU pixel conversion helpers.
//
// Used by:
//  * the media source, to deliver the negotiated media type (NV12 or RGB32 at any
//    advertised resolution) from the NV12 frame published by the desktop app;
//  * the desktop app, for the preview image and for the CPU fallback path when
//    the GPU video processor is not available.
//
// Colorimetry: BT.709, limited ("studio") range 16-235 - matching the
// MF_MT_YUV_MATRIX / MF_MT_VIDEO_NOMINAL_RANGE attributes advertised by the
// media source and the D3D11 video processor output color space.
//
// Portable C++ (no Windows headers) so it can be unit tested anywhere.
#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

namespace screencam {

struct Rect32 { int32_t x, y, w, h; };

enum class ScaleMode : int32_t
{
    Fit = 0,      // preserve aspect ratio, letterbox / pillarbox with black
    Stretch = 1,  // fill the whole frame, distorting the aspect ratio
    Fill = 2,     // preserve aspect ratio, crop the source to fill the frame
};

inline uint8_t Clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Destination rectangle (even aligned) for drawing a srcW x srcH image into a
// dstW x dstH frame using ScaleMode::Fit.
inline Rect32 ComputeFitRect(int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH)
{
    Rect32 r{ 0, 0, dstW, dstH };
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return r;
    // Compare aspect ratios without floating point: srcW/srcH vs dstW/dstH
    if ((int64_t)srcW * dstH > (int64_t)dstW * srcH)
    {
        // source is wider -> full width, reduced height
        int32_t h = (int32_t)((int64_t)dstW * srcH / srcW);
        h = std::max<int32_t>(2, h & ~1);
        r.h = h;
        r.y = ((dstH - h) / 2) & ~1;
    }
    else
    {
        int32_t w = (int32_t)((int64_t)dstH * srcW / srcH);
        w = std::max<int32_t>(2, w & ~1);
        r.w = w;
        r.x = ((dstW - w) / 2) & ~1;
    }
    return r;
}

// Source rectangle (within srcW x srcH) to crop so that it has the aspect ratio
// of dstW x dstH (ScaleMode::Fill).
inline Rect32 ComputeFillSourceRect(int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH)
{
    Rect32 r{ 0, 0, srcW, srcH };
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return r;
    if ((int64_t)srcW * dstH > (int64_t)dstW * srcH)
    {
        int32_t w = (int32_t)((int64_t)srcH * dstW / dstH);
        w = std::max<int32_t>(1, w);
        r.w = w;
        r.x = (srcW - w) / 2;
    }
    else
    {
        int32_t h = (int32_t)((int64_t)srcW * dstH / dstW);
        h = std::max<int32_t>(1, h);
        r.h = h;
        r.y = (srcH - h) / 2;
    }
    return r;
}

// ---------------------------------------------------------------------------
// NV12 helpers
// ---------------------------------------------------------------------------
inline void FillNv12(uint8_t* y, int32_t yPitch, uint8_t* uv, int32_t uvPitch,
                     int32_t w, int32_t h, uint8_t Y, uint8_t U, uint8_t V)
{
    for (int32_t row = 0; row < h; ++row)
        std::memset(y + (size_t)row * yPitch, Y, (size_t)w);
    for (int32_t row = 0; row < h / 2; ++row)
    {
        uint8_t* p = uv + (size_t)row * uvPitch;
        for (int32_t x = 0; x + 1 < w; x += 2) { p[x] = U; p[x + 1] = V; }
    }
}

inline void FillNv12Black(uint8_t* y, int32_t yPitch, uint8_t* uv, int32_t uvPitch, int32_t w, int32_t h)
{
    FillNv12(y, yPitch, uv, uvPitch, w, h, 16, 128, 128);
}

// "No signal" pattern: dark slate background with a subtle diagonal stripe
// band, so users can tell the camera is alive but the app is not sending video.
inline void FillNv12NoSignal(uint8_t* y, int32_t yPitch, uint8_t* uv, int32_t uvPitch,
                             int32_t w, int32_t h, uint32_t phase)
{
    for (int32_t row = 0; row < h; ++row)
    {
        uint8_t* p = y + (size_t)row * yPitch;
        const bool band = row > h * 2 / 5 && row < h * 3 / 5;
        for (int32_t x = 0; x < w; ++x)
        {
            uint8_t v = 40;
            if (band && (((uint32_t)(x + row) + phase) / 24) % 2 == 0)
                v = 60;
            p[x] = v;
        }
    }
    for (int32_t row = 0; row < h / 2; ++row)
    {
        uint8_t* p = uv + (size_t)row * uvPitch;
        for (int32_t x = 0; x + 1 < w; x += 2) { p[x] = 136; p[x + 1] = 124; }
    }
}

// Scales an NV12 image (nearest neighbour) into dstRect of an NV12 frame. If the
// sizes match it degenerates into plain row copies. dstRect must be even aligned.
inline void ScaleNv12IntoRect(const uint8_t* sy, int32_t syPitch, const uint8_t* suv, int32_t suvPitch,
                              int32_t sw, int32_t sh,
                              uint8_t* dy, int32_t dyPitch, uint8_t* duv, int32_t duvPitch,
                              Rect32 r)
{
    if (sw <= 0 || sh <= 0 || r.w <= 0 || r.h <= 0)
        return;
    if (r.w == sw && r.h == sh)
    {
        for (int32_t row = 0; row < sh; ++row)
            std::memcpy(dy + (size_t)(r.y + row) * dyPitch + r.x, sy + (size_t)row * syPitch, (size_t)sw);
        for (int32_t row = 0; row < sh / 2; ++row)
            std::memcpy(duv + (size_t)(r.y / 2 + row) * duvPitch + r.x, suv + (size_t)row * suvPitch, (size_t)sw);
        return;
    }
    // Fixed point 16.16 steps
    const uint32_t xStep = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)r.w);
    const uint32_t yStep = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)r.h);
    std::vector<int32_t> xmap((size_t)r.w);
    for (int32_t x = 0; x < r.w; ++x)
        xmap[(size_t)x] = std::min<int32_t>(sw - 1, (int32_t)(((uint64_t)x * xStep) >> 16));
    for (int32_t row = 0; row < r.h; ++row)
    {
        const int32_t srow = std::min<int32_t>(sh - 1, (int32_t)(((uint64_t)row * yStep) >> 16));
        const uint8_t* s = sy + (size_t)srow * syPitch;
        uint8_t* d = dy + (size_t)(r.y + row) * dyPitch + r.x;
        for (int32_t x = 0; x < r.w; ++x)
            d[x] = s[xmap[(size_t)x]];
    }
    const int32_t cw = r.w / 2, ch = r.h / 2, scw = sw / 2, sch = sh / 2;
    if (scw <= 0 || sch <= 0)
        return;
    const uint32_t cxStep = (uint32_t)(((uint64_t)scw << 16) / (uint32_t)cw);
    const uint32_t cyStep = (uint32_t)(((uint64_t)sch << 16) / (uint32_t)ch);
    for (int32_t row = 0; row < ch; ++row)
    {
        const int32_t srow = std::min<int32_t>(sch - 1, (int32_t)(((uint64_t)row * cyStep) >> 16));
        const uint16_t* s = reinterpret_cast<const uint16_t*>(suv + (size_t)srow * suvPitch);
        uint8_t* drow = duv + (size_t)(r.y / 2 + row) * duvPitch + r.x;
        for (int32_t x = 0; x < cw; ++x)
        {
            const int32_t sx = std::min<int32_t>(scw - 1, (int32_t)(((uint64_t)x * cxStep) >> 16));
            std::memcpy(drow + (size_t)x * 2, &s[sx], 2);
        }
    }
}

// Converts NV12 -> BGRA (alpha = 255), nearest-neighbour scaled into dstRect of
// a BGRA image. Pixels outside dstRect are not touched.
inline void Nv12ToBgraIntoRect(const uint8_t* sy, int32_t syPitch, const uint8_t* suv, int32_t suvPitch,
                               int32_t sw, int32_t sh,
                               uint8_t* dst, int32_t dstPitch, Rect32 r)
{
    if (sw <= 0 || sh <= 0 || r.w <= 0 || r.h <= 0)
        return;
    const uint32_t xStep = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)r.w);
    const uint32_t yStep = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)r.h);
    std::vector<int32_t> xmap((size_t)r.w);
    for (int32_t x = 0; x < r.w; ++x)
        xmap[(size_t)x] = std::min<int32_t>(sw - 1, (int32_t)(((uint64_t)x * xStep) >> 16));
    for (int32_t row = 0; row < r.h; ++row)
    {
        const int32_t srow = std::min<int32_t>(sh - 1, (int32_t)(((uint64_t)row * yStep) >> 16));
        const uint8_t* ys = sy + (size_t)srow * syPitch;
        const uint8_t* uvs = suv + (size_t)(srow / 2) * suvPitch;
        uint8_t* d = dst + (size_t)(r.y + row) * dstPitch + (size_t)r.x * 4;
        for (int32_t x = 0; x < r.w; ++x)
        {
            const int32_t sx = xmap[(size_t)x];
            const int C = (int)ys[sx] - 16;
            const int D = (int)uvs[sx & ~1] - 128;
            const int E = (int)uvs[(sx & ~1) + 1] - 128;
            d[0] = Clamp255((298 * C + 541 * D + 128) >> 8);            // B
            d[1] = Clamp255((298 * C - 55 * D - 136 * E + 128) >> 8);   // G
            d[2] = Clamp255((298 * C + 459 * E + 128) >> 8);            // R
            d[3] = 255;
            d += 4;
        }
    }
}

inline void FillBgra(uint8_t* dst, int32_t pitch, int32_t w, int32_t h, uint8_t b, uint8_t g, uint8_t r)
{
    for (int32_t row = 0; row < h; ++row)
    {
        uint8_t* d = dst + (size_t)row * pitch;
        for (int32_t x = 0; x < w; ++x) { d[0] = b; d[1] = g; d[2] = r; d[3] = 255; d += 4; }
    }
}

// Converts a BGRA image (source rectangle srcRect) into dstRect of an NV12
// frame with nearest-neighbour scaling. Chroma is the average of the 2x2 block.
inline void BgraToNv12IntoRect(const uint8_t* src, int32_t srcPitch, Rect32 srcRect,
                               uint8_t* dy, int32_t dyPitch, uint8_t* duv, int32_t duvPitch,
                               Rect32 r)
{
    if (srcRect.w <= 0 || srcRect.h <= 0 || r.w < 2 || r.h < 2)
        return;
    const uint32_t xStep = (uint32_t)(((uint64_t)srcRect.w << 16) / (uint32_t)r.w);
    const uint32_t yStep = (uint32_t)(((uint64_t)srcRect.h << 16) / (uint32_t)r.h);
    std::vector<int32_t> xmap((size_t)r.w);
    for (int32_t x = 0; x < r.w; ++x)
        xmap[(size_t)x] = srcRect.x + std::min<int32_t>(srcRect.w - 1, (int32_t)(((uint64_t)x * xStep) >> 16));

    for (int32_t row = 0; row < r.h; row += 2)
    {
        const int32_t s0 = srcRect.y + std::min<int32_t>(srcRect.h - 1, (int32_t)(((uint64_t)row * yStep) >> 16));
        const int32_t s1 = srcRect.y + std::min<int32_t>(srcRect.h - 1, (int32_t)(((uint64_t)(row + 1) * yStep) >> 16));
        const uint8_t* p0 = src + (size_t)s0 * srcPitch;
        const uint8_t* p1 = src + (size_t)s1 * srcPitch;
        uint8_t* y0 = dy + (size_t)(r.y + row) * dyPitch + r.x;
        uint8_t* y1 = y0 + dyPitch;
        uint8_t* c = duv + (size_t)((r.y + row) / 2) * duvPitch + r.x;
        for (int32_t x = 0; x < r.w; x += 2)
        {
            const uint8_t* a = p0 + (size_t)xmap[(size_t)x] * 4;
            const uint8_t* b = p0 + (size_t)xmap[(size_t)x + 1] * 4;
            const uint8_t* e = p1 + (size_t)xmap[(size_t)x] * 4;
            const uint8_t* f = p1 + (size_t)xmap[(size_t)x + 1] * 4;
            y0[x]     = (uint8_t)(16 + ((47 * a[2] + 157 * a[1] + 16 * a[0] + 128) >> 8));
            y0[x + 1] = (uint8_t)(16 + ((47 * b[2] + 157 * b[1] + 16 * b[0] + 128) >> 8));
            y1[x]     = (uint8_t)(16 + ((47 * e[2] + 157 * e[1] + 16 * e[0] + 128) >> 8));
            y1[x + 1] = (uint8_t)(16 + ((47 * f[2] + 157 * f[1] + 16 * f[0] + 128) >> 8));
            const int R = (a[2] + b[2] + e[2] + f[2] + 2) >> 2;
            const int G = (a[1] + b[1] + e[1] + f[1] + 2) >> 2;
            const int B = (a[0] + b[0] + e[0] + f[0] + 2) >> 2;
            c[x]     = Clamp255(128 + ((-26 * R - 87 * G + 112 * B + 128) >> 8));
            c[x + 1] = Clamp255(128 + ((112 * R - 102 * G - 10 * B + 128) >> 8));
        }
    }
}

} // namespace screencam
