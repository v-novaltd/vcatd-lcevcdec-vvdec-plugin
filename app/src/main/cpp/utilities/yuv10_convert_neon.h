/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// AArch64 NEON conversion from planar I420 10-bit to display formats.
// No scalar fallback: this plugin targets arm64-v8a only.

#ifndef VCAT_YUV10_CONVERT_NEON_H
#define VCAT_YUV10_CONVERT_NEON_H

#include "slice_thread_pool.h"

#include <cstddef>
#include <cstdint>

namespace vcat::yuv {

// A 10-bit LE plane; samples occupy the low bits of 16-bit words.
struct Plane10
{
    const uint8_t* data; // Reinterpreted as uint16_t samples.
    size_t rowByteStride;
};

// Downshift planar I420 10-bit to NV12/NV21. `nv21` must match the buffer's physical Cb/Cr order
void convertI420P10ToSemiPlanar8(SliceThreadPool& pool, Plane10 y, Plane10 u, Plane10 v,
                                 uint8_t* dstY, size_t dstYStride, uint8_t* dstChroma,
                                 size_t dstChromaStride, uint32_t width, uint32_t height, bool nv21);

// Losslessly repack planar I420 10-bit as P010. `swapCbCr` must match the
// destination buffer's physical chroma-word order.
void convertI420P10ToP010(SliceThreadPool& pool, Plane10 y, Plane10 u, Plane10 v, uint8_t* dstY,
                          size_t dstYStride, uint8_t* dstChroma, size_t dstChromaStride,
                          uint32_t width, uint32_t height, bool swapCbCr);

} // namespace vcat::yuv

#endif // VCAT_YUV10_CONVERT_NEON_H
