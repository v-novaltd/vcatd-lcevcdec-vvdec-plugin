/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "yuv10_convert_neon.h"

#include <arm_neon.h>

#include <cstring>

namespace vcat::yuv {

namespace {

    // 16 samples per 8-bit conversion iteration.
    constexpr uint32_t kWide = 16;
    constexpr uint32_t kWideMask = kWide - 1u; // width & ~kWideMask rounds down, no divide.

    // P010 conversion processes eight 16-bit samples per iteration.
    constexpr uint32_t kWide16 = 8;
    constexpr uint32_t kWideMask16 = kWide16 - 1u;

    // Downshift with rounding and saturation; stage the short tail for NEON.
    __attribute__((always_inline)) inline void downshiftSpan(const uint16_t* in, uint8_t* out, uint32_t count)
    {
        const uint32_t main = count & ~kWideMask;
        uint32_t x = 0;
        for (; x < main; x += kWide) {
            const uint8x8_t lo = vqrshrn_n_u16(vld1q_u16(in + x), 2);
            const uint8x8_t hi = vqrshrn_n_u16(vld1q_u16(in + x + 8), 2);
            vst1q_u8(out + x, vcombine_u8(lo, hi));
        }
        if (x == count) {
            return;
        }
        const uint32_t remaining = count - x;
        uint16_t stagedIn[kWide] = {};
        uint8_t stagedOut[kWide];
        std::memcpy(stagedIn, in + x, remaining * sizeof(uint16_t));
        const uint8x8_t lo = vqrshrn_n_u16(vld1q_u16(stagedIn), 2);
        const uint8x8_t hi = vqrshrn_n_u16(vld1q_u16(stagedIn + 8), 2);
        vst1q_u8(stagedOut, vcombine_u8(lo, hi));
        std::memcpy(out + x, stagedOut, remaining);
    }

    // P010 stores samples in the high bits, so conversion is a lossless << 6.
    __attribute__((always_inline)) inline void shiftSpan(const uint16_t* in, uint16_t* out, uint32_t count)
    {
        const uint32_t main = count & ~kWideMask16;
        uint32_t x = 0;
        for (; x < main; x += kWide16) {
            vst1q_u16(out + x, vshlq_n_u16(vld1q_u16(in + x), 6));
        }
        if (x == count) {
            return;
        }
        const uint32_t remaining = count - x;
        uint16_t stagedIn[kWide16] = {};
        uint16_t stagedOut[kWide16];
        std::memcpy(stagedIn, in + x, remaining * sizeof(uint16_t));
        vst1q_u16(stagedOut, vshlq_n_u16(vld1q_u16(stagedIn), 6));
        std::memcpy(out + x, stagedOut, remaining * sizeof(uint16_t));
    }

} // namespace

namespace {

    // Matching luma/chroma row ranges. The final slice includes an odd luma row.
    struct Slice
    {
        uint32_t lumaBegin, lumaEnd;
        uint32_t chromaBegin, chromaEnd;
    };

    // Split chroma rows evenly; derive matching luma ranges.
    inline Slice makeSlice(uint32_t sliceIndex, uint32_t sliceCount, uint32_t height, uint32_t chromaHeight)
    {
        const uint32_t chromaBegin = (chromaHeight * sliceIndex) / sliceCount;
        const uint32_t chromaEnd = (chromaHeight * (sliceIndex + 1)) / sliceCount;
        const bool isLast = sliceIndex + 1 == sliceCount;
        return {chromaBegin * 2, isLast ? height : chromaEnd * 2, chromaBegin, chromaEnd};
    }

} // namespace

void convertI420P10ToSemiPlanar8(SliceThreadPool& pool, Plane10 y, Plane10 u, Plane10 v,
                                 uint8_t* dstY, size_t dstYStride, uint8_t* dstChroma,
                                 size_t dstChromaStride, uint32_t width, uint32_t height, bool nv21)
{
    const uint32_t chromaWidth = (width + 1) >> 1;
    const uint32_t chromaHeight = (height + 1) >> 1;
    const uint32_t chromaMain = chromaWidth & ~kWideMask;
    const uint32_t sliceCount = pool.threadCount();

    pool.run([&](uint32_t sliceIndex) {
        const Slice slice = makeSlice(sliceIndex, sliceCount, height, chromaHeight);

        const auto* yRow = y.data + static_cast<size_t>(slice.lumaBegin) * y.rowByteStride;
        auto* dstYRow = dstY + static_cast<size_t>(slice.lumaBegin) * dstYStride;
        for (uint32_t row = slice.lumaBegin; row < slice.lumaEnd;
             ++row, yRow += y.rowByteStride, dstYRow += dstYStride) {
            downshiftSpan(reinterpret_cast<const uint16_t*>(yRow), dstYRow, width);
        }

        const auto* uRow = u.data + static_cast<size_t>(slice.chromaBegin) * u.rowByteStride;
        const auto* vRow = v.data + static_cast<size_t>(slice.chromaBegin) * v.rowByteStride;
        auto* dstChromaRow = dstChroma + static_cast<size_t>(slice.chromaBegin) * dstChromaStride;
        for (uint32_t row = slice.chromaBegin; row < slice.chromaEnd; ++row, uRow += u.rowByteStride,
                      vRow += v.rowByteStride, dstChromaRow += dstChromaStride) {
            const auto* inU = reinterpret_cast<const uint16_t*>(uRow);
            const auto* inV = reinterpret_cast<const uint16_t*>(vRow);
            uint32_t x = 0;
            for (; x < chromaMain; x += kWide) {
                const uint8x16_t u8 = vcombine_u8(vqrshrn_n_u16(vld1q_u16(inU + x), 2),
                                                  vqrshrn_n_u16(vld1q_u16(inU + x + 8), 2));
                const uint8x16_t v8 = vcombine_u8(vqrshrn_n_u16(vld1q_u16(inV + x), 2),
                                                  vqrshrn_n_u16(vld1q_u16(inV + x + 8), 2));
                // Vector order selects NV12 or NV21 chroma order.
                const uint8x16x2_t pair = nv21 ? uint8x16x2_t{{v8, u8}} : uint8x16x2_t{{u8, v8}};
                vst2q_u8(dstChromaRow + x * 2, pair);
            }
            if (x == chromaWidth) {
                continue;
            }
            const uint32_t remaining = chromaWidth - x;
            uint16_t stagedU[kWide] = {}, stagedV[kWide] = {};
            uint8_t stagedOut[kWide * 2];
            std::memcpy(stagedU, inU + x, remaining * sizeof(uint16_t));
            std::memcpy(stagedV, inV + x, remaining * sizeof(uint16_t));
            const uint8x16_t u8 = vcombine_u8(vqrshrn_n_u16(vld1q_u16(stagedU), 2),
                                              vqrshrn_n_u16(vld1q_u16(stagedU + 8), 2));
            const uint8x16_t v8 = vcombine_u8(vqrshrn_n_u16(vld1q_u16(stagedV), 2),
                                              vqrshrn_n_u16(vld1q_u16(stagedV + 8), 2));
            const uint8x16x2_t stagedPair = nv21 ? uint8x16x2_t{{v8, u8}} : uint8x16x2_t{{u8, v8}};
            vst2q_u8(stagedOut, stagedPair);
            std::memcpy(dstChromaRow + x * 2, stagedOut, remaining * 2);
        }
    });
}

void convertI420P10ToP010(SliceThreadPool& pool, Plane10 y, Plane10 u, Plane10 v, uint8_t* dstY,
                          size_t dstYStride, uint8_t* dstChroma, size_t dstChromaStride,
                          uint32_t width, uint32_t height, bool swapCbCr)
{
    const uint32_t chromaWidth = (width + 1) >> 1;
    const uint32_t chromaHeight = (height + 1) >> 1;
    const uint32_t chromaMain = chromaWidth & ~kWideMask16;
    const uint32_t sliceCount = pool.threadCount();

    pool.run([&](uint32_t sliceIndex) {
        const Slice slice = makeSlice(sliceIndex, sliceCount, height, chromaHeight);

        const auto* yRow = y.data + static_cast<size_t>(slice.lumaBegin) * y.rowByteStride;
        auto* dstYRow = dstY + static_cast<size_t>(slice.lumaBegin) * dstYStride;
        for (uint32_t row = slice.lumaBegin; row < slice.lumaEnd;
             ++row, yRow += y.rowByteStride, dstYRow += dstYStride) {
            shiftSpan(reinterpret_cast<const uint16_t*>(yRow), reinterpret_cast<uint16_t*>(dstYRow), width);
        }

        const auto* uRow = u.data + static_cast<size_t>(slice.chromaBegin) * u.rowByteStride;
        const auto* vRow = v.data + static_cast<size_t>(slice.chromaBegin) * v.rowByteStride;
        auto* dstChromaRow = dstChroma + static_cast<size_t>(slice.chromaBegin) * dstChromaStride;
        for (uint32_t row = slice.chromaBegin; row < slice.chromaEnd; ++row, uRow += u.rowByteStride,
                      vRow += v.rowByteStride, dstChromaRow += dstChromaStride) {
            const auto* inU = reinterpret_cast<const uint16_t*>(uRow);
            const auto* inV = reinterpret_cast<const uint16_t*>(vRow);
            auto* outChroma = reinterpret_cast<uint16_t*>(dstChromaRow);
            uint32_t x = 0;
            for (; x < chromaMain; x += kWide16) {
                const uint16x8_t u16 = vshlq_n_u16(vld1q_u16(inU + x), 6);
                const uint16x8_t v16 = vshlq_n_u16(vld1q_u16(inV + x), 6);
                // Vector order selects the P010 chroma-word order.
                const uint16x8x2_t pair = swapCbCr ? uint16x8x2_t{{v16, u16}} : uint16x8x2_t{{u16, v16}};
                vst2q_u16(outChroma + x * 2, pair);
            }
            if (x == chromaWidth) {
                continue;
            }
            const uint32_t remaining = chromaWidth - x;
            uint16_t stagedU[kWide16] = {}, stagedV[kWide16] = {};
            uint16_t stagedOut[kWide16 * 2];
            std::memcpy(stagedU, inU + x, remaining * sizeof(uint16_t));
            std::memcpy(stagedV, inV + x, remaining * sizeof(uint16_t));
            const uint16x8_t u16 = vshlq_n_u16(vld1q_u16(stagedU), 6);
            const uint16x8_t v16 = vshlq_n_u16(vld1q_u16(stagedV), 6);
            const uint16x8x2_t stagedPair =
                swapCbCr ? uint16x8x2_t{{v16, u16}} : uint16x8x2_t{{u16, v16}};
            vst2q_u16(stagedOut, stagedPair);
            std::memcpy(outChroma + x * 2, stagedOut, remaining * 2 * sizeof(uint16_t));
        }
    });
}

} // namespace vcat::yuv
