/*
 * VCAT (Video Codec Acid Test)
 *
 * SPDX-FileCopyrightText: Copyright (C) 2020-2025 VCAT authors and RoncaTech
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of VCAT.
 *
 * VCAT is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VCAT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VCAT. If not, see <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * For proprietary/commercial use cases, a written GPL-3.0 waiver or
 * a separate commercial license is required from RoncaTech LLC.
 * All VCAT artwork is owned exclusively by RoncaTech LLC. Use of VCAT logos and artwork is
 * permitted for the purpose of discussing, documenting, or promoting VCAT itself. Any other use
 * requires prior written permission from RoncaTech LLC. Contact: legal@roncatech.com •
 * https://roncatech.com/legal
 */
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 *
 * Modified by V-Nova International Limited, to support integration with LCEVCdec.
 * V-Nova's modifications are licensed under GPL-3.0-or-later.
 */
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 *
 * Modified by V-Nova International Limited, to support integration with LCEVCdec.
 * V-Nova's modifications are licensed under GPL-3.0-or-later.
 */

// VVDEC base decode, LCEVC enhancement, and SurfaceControl presentation at ExoPlayer timing.
//
// Output pictures are allocated with LCEVC_AllocPictureExternal() on AHardwareBuffers from the
// presenter's pool, so the decoder writes straight into the buffer that is put on screen. Where the
// decoder cannot import a buffer, it decodes into a managed picture that is copied into a
// CPU-written buffer instead.

#include "LCEVC/build_config.h"
#include "LCEVC/extract/extract.h"
#include "LCEVC/lcevc_dec.h"
#include "utilities/slice_thread_pool.h"
#include "utilities/surface_presenter.h"
#include "utilities/trace_scope.h"
#include "utilities/yuv10_convert_neon.h"
#if VN_SDK_FEATURE(TRACING_SYSTEM_SINK)
#include "LCEVC/common/diagnostics.h"
#include "LCEVC/utility/perfetto_sdk.h"
#endif

extern "C"
{
#include "vvdec/vvdec.h"
}

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#define LOG_TAG "lcevc_vvdec_jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#if VN_SDK_FEATURE(TRACING_SYSTEM_SINK)
#define LCEVC_JNI_TRACE_SCOPE(name) VNTraceScopedNamed(name)
#define LCEVC_JNI_TRACE_SCOPE_ARGS(name, ...) VNTraceScopedNamedArgs(name, __VA_ARGS__)
#else
#define LCEVC_JNI_TRACE_SCOPE(name) ((void)0)
#define LCEVC_JNI_TRACE_SCOPE_ARGS(name, ...) ((void)0)
#endif

static constexpr size_t kMaxPendingPackets = 16;
// Completed outputs waiting for Java. Each holds a presenter pool buffer, so this
// must stay within the pool's budget for queued frames (see SurfacePresenter).
static constexpr size_t kMaxReadyOutputs = 6;
static constexpr uint32_t kEnhancementBufBytes = 8u * 1024u * 1024u;
static constexpr jint kDequeueError = -1;
static constexpr jint kDequeueEndOfStream = -2;

static const char* lcevcError(LCEVC_ReturnCode rc)
{
    switch (rc) {
        case LCEVC_Success: return "Success";
        case LCEVC_Again: return "Again";
        case LCEVC_NotFound: return "NotFound";
        case LCEVC_Error: return "Error";
        case LCEVC_Uninitialized: return "Uninitialized";
        case LCEVC_Initialized: return "Initialized";
        case LCEVC_InvalidParam: return "InvalidParam";
        case LCEVC_NotSupported: return "NotSupported";
        case LCEVC_Flushed: return "Flushed";
        case LCEVC_Timeout: return "Timeout";
        default: return "Unknown";
    }
}

// A finished LCEVC output picture, ready to be dequeued/rendered by the Java layer.
struct OutputHolder
{
    LCEVC_DecoderHandle lcevc{};
    LCEVC_PictureHandle pic{};
    uint32_t width = 0;
    uint32_t height = 0;
    LCEVC_ColorFormat format = LCEVC_ColorFormat_Unknown; // Meaningful only for managed pictures.
    int64_t pts = -1;

    // Either `buffer` (ready to present) or a managed `pic` still to be copied into one.
    AHardwareBuffer* buffer = nullptr;
    std::shared_ptr<SurfacePresenter> presenter;
    unsigned generation = 0;
};

// Free list of managed pictures, keyed by size and format.
struct PicturePool
{
    struct Entry
    {
        LCEVC_PictureHandle pic;
        uint32_t width;
        uint32_t height;
        LCEVC_ColorFormat format;
    };
    std::vector<Entry> free;
    uint32_t maxCount = 0;

    LCEVC_ReturnCode acquire(LCEVC_DecoderHandle lcevc, const LCEVC_PictureDesc& desc,
                             LCEVC_PictureHandle* outPic)
    {
        for (auto it = free.begin(); it != free.end(); ++it) {
            if (it->width == desc.width && it->height == desc.height && it->format == desc.colorFormat) {
                *outPic = it->pic;
                free.erase(it);
                return LCEVC_Success;
            }
        }
        // Evict a stale entry so the pool adapts to format or size changes.
        if (!free.empty()) {
            LCEVC_FreePicture(lcevc, free.back().pic);
            free.pop_back();
        }
        return LCEVC_AllocPicture(lcevc, &desc, outPic);
    }

    void release(LCEVC_DecoderHandle lcevc, LCEVC_PictureHandle pic, uint32_t width,
                 uint32_t height, LCEVC_ColorFormat format)
    {
        if (free.size() < maxCount) {
            free.push_back({pic, width, height, format});
        } else {
            LCEVC_FreePicture(lcevc, pic);
        }
    }

    void releaseAll(LCEVC_DecoderHandle lcevc)
    {
        for (auto& e : free) {
            LCEVC_FreePicture(lcevc, e.pic);
        }
        free.clear();
    }
};

struct NativeCtx
{
    vvdecDecoder* dec = nullptr;
    LCEVC_DecoderHandle lcevc{};

    // Both pipelines receive before presentation; output import formats differ.
    bool vulkanPipeline = false;
    std::shared_ptr<SurfacePresenter> presenter;
    // Set once the decoder rejects a hardware buffer; later pictures are copied instead.
    bool externalOutputFailed = false;
    uint32_t maxOutputWidth = 0;
    uint32_t maxOutputHeight = 0;
    std::unordered_map<uintptr_t, AHardwareBuffer*> externalOutputs;
    // Vulkan output pictures stay bound to their pool buffer between frames, so each buffer is
    // imported into Vulkan once rather than on every decode.
    std::unordered_map<AHardwareBuffer*, LCEVC_PictureHandle> boundPictures;

    std::optional<bool> p010Supported;

    // Managed input and copied-output picture pools.
    PicturePool basePicturePool;
    PicturePool outputPicturePool;

    // Reused pool for parallel 10-bit conversion.
    std::unique_ptr<SliceThreadPool> convertPool;

    vvdecParams params{};
    struct BaseJob
    {
        LCEVC_PictureHandle pic{};
        uint64_t pts = 0;
        bool baseSent = false;
    } baseJob;
    bool haveBaseJob = false;
    bool baseDrained = false;
    int error = 0;
    unsigned generation = 0;
    std::deque<int64_t> expected;
    std::unordered_map<int64_t, bool> decodeOnly;
    std::deque<OutputHolder*> ready; // Completed LCEVC output pictures

    uint8_t* lcevcData = nullptr;

    // Mutexes allow specific control of the two libraries to manage scheduling for efficiency
    std::shared_mutex lifecycleMutex;
    std::mutex lcevcMutex;
    std::mutex vvdecMutex;

    ANativeWindow* win = nullptr;
    bool eos = false;
};

static LCEVC_NALFormat detectNalFormat(const uint8_t* d, uint32_t n)
{
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) {
        return LCEVC_NALFormat_AnnexB;
    }
    if (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1) {
        return LCEVC_NALFormat_AnnexB;
    }
    return LCEVC_NALFormat_LengthPrefix;
}

// All helpers below assume ctx->lcevcMutex is held by the caller.

// Copy the base into managed LCEVC storage; no VVDEC frame crosses the API boundary.
static bool lcevcCopyBase(NativeCtx* ctx, vvdecFrame* frame, LCEVC_PictureHandle* outPic)
{
    LCEVC_JNI_TRACE_SCOPE_ARGS("jni:vcat.lcevc.copyBase", "timestamp", frame->cts);
    if (frame->colorFormat != VVDEC_CF_YUV420_PLANAR || frame->numPlanes < 3 ||
        (frame->bitDepth != 8 && frame->bitDepth != 10)) {
        return false;
    }
    LCEVC_PictureDesc desc{};
    const bool nv12 = !ctx->vulkanPipeline && frame->bitDepth == 8;
    auto fmt = frame->bitDepth == 8 ? (nv12 ? LCEVC_NV12_8 : LCEVC_I420_8) : LCEVC_I420_10_LE;
    if (LCEVC_DefaultPictureDesc(&desc, fmt, frame->width, frame->height) != LCEVC_Success ||
        ctx->basePicturePool.acquire(ctx->lcevc, desc, outPic) != LCEVC_Success) {
        return false;
    }
    LCEVC_PictureLockHandle lock{};
    if (LCEVC_LockPicture(ctx->lcevc, *outPic, LCEVC_Access_Write, &lock) != LCEVC_Success) {
        LCEVC_FreePicture(ctx->lcevc, *outPic);
        return false;
    }
    bool ok = true;
    for (uint32_t p = 0; p < (nv12 ? 1u : 3u) && ok; ++p) {
        LCEVC_PicturePlaneDesc dst{};
        const auto& src = frame->planes[p];
        const uint32_t bytes = src.width * (frame->bitDepth > 8 ? 2 : 1);
        ok = LCEVC_GetPictureLockPlaneDesc(ctx->lcevc, lock, p, &dst) == LCEVC_Success &&
             dst.firstSample && src.ptr && dst.rowByteStride >= bytes && src.stride >= bytes;
        if (ok) {
            for (uint32_t y = 0; y < src.height; ++y) {
                std::memcpy(dst.firstSample + y * dst.rowByteStride, src.ptr + y * src.stride, bytes);
            }
        }
    }
    if (nv12 && ok) {
        LCEVC_PicturePlaneDesc dst{};
        const auto& u = frame->planes[1];
        const auto& v = frame->planes[2];
        ok = LCEVC_GetPictureLockPlaneDesc(ctx->lcevc, lock, 1, &dst) == LCEVC_Success &&
             dst.firstSample && dst.rowByteStride >= u.width * 2 && u.ptr && v.ptr;
        if (ok) {
            for (uint32_t y = 0; y < u.height; ++y) {
                auto* row = dst.firstSample + y * dst.rowByteStride;
                for (uint32_t x = 0; x < u.width; ++x) {
                    row[2 * x] = u.ptr[y * u.stride + x];
                    row[2 * x + 1] = v.ptr[y * v.stride + x];
                }
            }
        }
    }
    LCEVC_UnlockPicture(ctx->lcevc, lock);
    if (!ok) {
        LCEVC_FreePicture(ctx->lcevc, *outPic);
    }
    return ok;
}

static void lcevcRecycleBases(NativeCtx* ctx)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.lcevc.recycleBases");
    LCEVC_PictureHandle base{};
    while (LCEVC_ReceiveDecoderBase(ctx->lcevc, &base) == LCEVC_Success) {
        LCEVC_PictureDesc desc{};
        if (LCEVC_GetPictureDesc(ctx->lcevc, base, &desc) == LCEVC_Success) {
            ctx->basePicturePool.release(ctx->lcevc, base, desc.width, desc.height, desc.colorFormat);
        } else {
            LCEVC_FreePicture(ctx->lcevc, base);
        }
    }
}

// Probe whether CPU-written P010 buffers can be allocated for 10-bit copies.
static bool probeP010Supported(NativeCtx* ctx, uint32_t width, uint32_t height)
{
    TRACE_SCOPE("jni:vcat.lcevc.probeP010");
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_YCbCr_P010;
    desc.usage = ctx->presenter->cpuBufferUsage();
    const bool supported = AHardwareBuffer_isSupported(&desc);
    LOGI("probe_p010_supported: %ux%u -> %s", width, height, supported ? "YES" : "no");
    return supported;
}

// Qualcomm gralloc allocates UBWC-compressed buffers for this vendor usage bit
// (GRALLOC_USAGE_PRIVATE_ALLOC_UBWC). Adreno's inline rotator only scans out compressed RGB
// buffers, so without it a rotated (landscape) video layer falls back to GPU composition.
static uint64_t gpuOutputVendorUsage()
{
    static const uint64_t usage = [] {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("ro.hardware.vulkan", value);
        return std::strcmp(value, "adreno") == 0 ? AHARDWAREBUFFER_USAGE_VENDOR_0 : 0;
    }();
    return usage;
}

// A hardware buffer the decoder writes directly, and the picture format it is decoded as.
struct ExternalOutput
{
    LCEVC_ColorFormat colorFormat;
    uint32_t bufferFormat;
    uint64_t usage;
};

static std::optional<ExternalOutput> chooseExternalOutput(NativeCtx* ctx, const LCEVC_PictureDesc& desc)
{
    if (ctx->vulkanPipeline) {
        // The decoder converts to RGB as it writes. YUV buffers commonly import into Vulkan only
        // as external formats, which a compute shader cannot write, whereas RGBA buffers import
        // as plain storage images that need no CPU access and can be scanned out directly.
        const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                               AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | gpuOutputVendorUsage();
        switch (desc.colorFormat) {
            case LCEVC_I420_8:
            case LCEVC_NV12_8:
                return ExternalOutput{LCEVC_RGBA_8, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, usage};
            case LCEVC_I420_10_LE:
                return ExternalOutput{LCEVC_RGBA_10_2_LE, AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM, usage};
            default: return std::nullopt;
        }
    }
    // The CPU pipeline writes 8-bit NV12 output in place; higher bit depths are copied.
    if (desc.colorFormat != LCEVC_NV12_8) {
        return std::nullopt;
    }
    return ExternalOutput{LCEVC_NV12_8, ctx->presenter->cpuYuv420Format(),
                          ctx->presenter->cpuBufferUsage()};
}

static void releaseBoundPicture(NativeCtx* ctx, AHardwareBuffer* buffer)
{
    auto bound = ctx->boundPictures.find(buffer);
    if (bound != ctx->boundPictures.end()) {
        LCEVC_FreePicture(ctx->lcevc, bound->second);
        ctx->boundPictures.erase(bound);
    }
}

// Back an output picture with a pool buffer. Returns the buffer, or nullptr with `exhausted` set
// while every pool buffer is in use, or with it clear when the decoder rejected the buffer.
static AHardwareBuffer* allocExternalPicture(NativeCtx* ctx, const LCEVC_PictureDesc& desc,
                                             const ExternalOutput& output,
                                             LCEVC_PictureHandle* picture, bool* exhausted)
{
    // A hardware buffer's dimensions are the picture's, so size each buffer to its picture.
    AHardwareBuffer* evicted = nullptr;
    AHardwareBuffer* buffer = ctx->presenter->acquireBuffer(desc.width, desc.height, output.bufferFormat,
                                                            output.usage, exhausted, &evicted);
    if (evicted) {
        releaseBoundPicture(ctx, evicted);
    }
    if (!buffer) {
        return nullptr;
    }
    LCEVC_PictureDesc externalDesc = desc;
    externalDesc.colorFormat = output.colorFormat;
    if (auto bound = ctx->boundPictures.find(buffer); bound != ctx->boundPictures.end()) {
        if (LCEVC_SetPictureDesc(ctx->lcevc, bound->second, &externalDesc) == LCEVC_Success) {
            *picture = bound->second;
            return buffer;
        }
        releaseBoundPicture(ctx, buffer);
    }
    LCEVC_PictureBufferDesc bufferDesc{};
    bufferDesc.accelBuffer.hdl = reinterpret_cast<uintptr_t>(buffer);
    bufferDesc.access = LCEVC_Access_Write;
    LCEVC_ReturnCode rc;
    {
        LCEVC_JNI_TRACE_SCOPE("jni:vcat.lcevc.allocPictureExternal");
        rc = LCEVC_AllocPictureExternal(ctx->lcevc, &externalDesc, &bufferDesc, nullptr, picture);
    }
    if (rc != LCEVC_Success) {
        ctx->presenter->recycleBuffer(buffer);
        return nullptr;
    }
    if (ctx->vulkanPipeline) {
        ctx->boundPictures.emplace(buffer, *picture);
    }
    return buffer;
}

// Discard a received output picture, returning any buffer behind it to the pool.
static void freeReceivedPicture(NativeCtx* ctx, LCEVC_PictureHandle picture)
{
    auto external = ctx->externalOutputs.find(picture.hdl);
    if (external != ctx->externalOutputs.end()) {
        ctx->presenter->recycleBuffer(external->second);
        const bool bound = ctx->boundPictures.count(external->second) != 0;
        ctx->externalOutputs.erase(external);
        if (bound) {
            return;
        }
    }
    LCEVC_FreePicture(ctx->lcevc, picture);
}

static LCEVC_ReturnCode lcevcSendOutputPicture(NativeCtx* ctx, uint64_t timestamp)
{
    LCEVC_JNI_TRACE_SCOPE_ARGS("jni:vcat.lcevc.sendOutput", "timestamp", timestamp);
    LCEVC_PictureDesc desc{};
    auto rc = LCEVC_PeekDecoderPictureDesc(ctx->lcevc, timestamp, &desc);
    if (rc != LCEVC_Success) {
        return rc;
    }

    ctx->maxOutputWidth = std::max(ctx->maxOutputWidth, desc.width);
    ctx->maxOutputHeight = std::max(ctx->maxOutputHeight, desc.height);

    LCEVC_PictureHandle picture{};
    AHardwareBuffer* buffer = nullptr;
    std::optional<ExternalOutput> external;
    if (!ctx->externalOutputFailed && ctx->presenter->ready()) {
        external = chooseExternalOutput(ctx, desc);
    }
    if (external) {
        bool exhausted = false;
        buffer = allocExternalPicture(ctx, desc, *external, &picture, &exhausted);
        if (exhausted) {
            return LCEVC_Again; // Wait for presentation to return a buffer.
        }
        // Older LCEVCdec only imports flexible YUV: retry with it before giving up.
        if (!buffer && !ctx->vulkanPipeline && external->bufferFormat != AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420) {
            LOGW("Explicit NV21 output not importable; retrying with flexible YUV");
            ctx->presenter->disableExplicitNv21();
            external->bufferFormat = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
            buffer = allocExternalPicture(ctx, desc, *external, &picture, &exhausted);
            if (exhausted) {
                return LCEVC_Again;
            }
        }
        if (!buffer) {
            LOGW("Decoder cannot write format 0x%x output buffers; copying output for this session",
                 external->bufferFormat);
            ctx->externalOutputFailed = true;
        }
    }
    if (!buffer) {
        rc = ctx->outputPicturePool.acquire(ctx->lcevc, desc, &picture);
        if (rc != LCEVC_Success) {
            return rc;
        }
    }
    rc = LCEVC_SendDecoderPicture(ctx->lcevc, picture);
    if (rc != LCEVC_Success) {
        if (buffer) {
            ctx->presenter->recycleBuffer(buffer);
        }
        if (!ctx->boundPictures.count(buffer)) {
            LCEVC_FreePicture(ctx->lcevc, picture);
        }
        return rc;
    }
    if (buffer) {
        ctx->externalOutputs.emplace(picture.hdl, buffer);
    }
    return LCEVC_Success;
}

// Decoder event callback. Runs on an LCEVCdec event thread: it must not call back
// into the decoder API, and must not take lcevcMutex.
static void lcevcEventCallback(LCEVC_DecoderHandle, LCEVC_Event event, LCEVC_PictureHandle,
                               const LCEVC_DecodeInformation*, const uint8_t* data,
                               uint32_t dataSize, void*)
{
    if (event == LCEVC_Log) {
        if (!data || dataSize == 0) {
            return;
        }

        int priority = ANDROID_LOG_INFO;
        switch (data[0]) {
            case '1': priority = ANDROID_LOG_FATAL; break;
            case '2': priority = ANDROID_LOG_ERROR; break;
            case '3': priority = ANDROID_LOG_WARN; break;
            case '4': priority = ANDROID_LOG_INFO; break;
            case '5': priority = ANDROID_LOG_DEBUG; break;
            case '6': priority = ANDROID_LOG_VERBOSE; break;
            default: break;
        }

        // Public LCEVC_Log payloads begin with "<level> ". They include a trailing
        // NUL today, but use the supplied size so this remains safe if that changes.
        uint32_t offset = dataSize >= 2 && data[1] == ' ' ? 2 : 0;
        uint32_t length = dataSize - offset;
        if (length > 0 && data[offset + length - 1] == '\0') {
            --length;
        }
        __android_log_print(priority, "LCEVCdec", "%.*s", static_cast<int>(length),
                            reinterpret_cast<const char*>(data + offset));
        return;
    }
}

// Copy 8-bit planar or NV12 source chroma into a semi-planar destination in its Cb/Cr order.
static void copyChroma8(const LCEVC_PicturePlaneDesc* src, bool srcSemiPlanar, uint8_t* dst,
                        size_t dstStride, uint32_t width, uint32_t height, bool swapCbCr)
{
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = dst + y * dstStride;
        if (srcSemiPlanar) {
            const uint8_t* uv = src[1].firstSample + y * src[1].rowByteStride;
            if (!swapCbCr) {
                std::memcpy(row, uv, width * 2);
                continue;
            }
            for (uint32_t x = 0; x < width; ++x) {
                row[2 * x] = uv[2 * x + 1];
                row[2 * x + 1] = uv[2 * x];
            }
            continue;
        }
        const uint8_t* cb = src[1].firstSample + y * src[1].rowByteStride;
        const uint8_t* cr = src[2].firstSample + y * src[2].rowByteStride;
        if (swapCbCr) {
            std::swap(cb, cr);
        }
        for (uint32_t x = 0; x < width; ++x) {
            row[2 * x] = cb[x];
            row[2 * x + 1] = cr[x];
        }
    }
}

// Copy a managed picture into a CPU-written presenter buffer, for output the decoder could not
// write into a hardware buffer itself. 10-bit output is repacked as P010 where supported, and
// otherwise downshifted to 8-bit.
static bool copyToPresenterBuffer(NativeCtx* ctx, OutputHolder* hold)
{
    TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer");
    const bool srcSemiPlanar = hold->format == LCEVC_NV12_8;
    if (hold->format != LCEVC_I420_8 && hold->format != LCEVC_NV12_8 && hold->format != LCEVC_I420_10_LE) {
        LOGE("copyToPresenterBuffer: unsupported picture format %d", hold->format);
        return false;
    }
    if (!ctx->presenter->ready()) {
        return false;
    }
    if (hold->format == LCEVC_I420_10_LE && !ctx->p010Supported.has_value()) {
        ctx->p010Supported = probeP010Supported(ctx, ctx->maxOutputWidth, ctx->maxOutputHeight);
    }
    const bool useP010 = hold->format == LCEVC_I420_10_LE && *ctx->p010Supported;
    const uint32_t format =
        useP010 ? AHARDWAREBUFFER_FORMAT_YCbCr_P010 : ctx->presenter->cpuYuv420Format();

    AHardwareBuffer* buffer;
    {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.acquireBuffer");
        buffer = ctx->presenter->acquireBuffer(ctx->maxOutputWidth, ctx->maxOutputHeight, format,
                                               ctx->presenter->cpuBufferUsage());
    }
    if (!buffer) {
        return false;
    }

    AHardwareBuffer_Planes planes{};
    {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.lockTarget");
        if (AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr,
                                       &planes) != 0 ||
            planes.planeCount < 3) {
            ctx->presenter->recycleBuffer(buffer);
            return false;
        }
    }
    // Chroma samples are adjacent words: 1 byte for NV12/NV21, 2 for P010.
    const uint32_t chromaWordBytes = useP010 ? 2u : 1u;
    auto* const cbPtr = static_cast<uint8_t*>(planes.planes[1].data);
    auto* const crPtr = static_cast<uint8_t*>(planes.planes[2].data);
    const bool semiPlanar = planes.planes[1].pixelStride == chromaWordBytes * 2 &&
                            planes.planes[2].pixelStride == chromaWordBytes * 2 &&
                            (crPtr == cbPtr + chromaWordBytes || cbPtr == crPtr + chromaWordBytes);
    if (!semiPlanar) {
        LOGE("copyToPresenterBuffer: buffer format 0x%x is not semi-planar", format);
        AHardwareBuffer_unlock(buffer, nullptr);
        ctx->presenter->recycleBuffer(buffer);
        return false;
    }
    const bool swapCbCr = (cbPtr == crPtr + chromaWordBytes); // Device writes Cr before Cb.

    LCEVC_PictureLockHandle sourceLock{};
    LCEVC_PicturePlaneDesc src[3]{};
    const uint32_t srcPlanes = srcSemiPlanar ? 2 : 3;
    bool sourceOk;
    {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.lockSource");
        sourceOk = LCEVC_LockPicture(ctx->lcevc, hold->pic, LCEVC_Access_Read, &sourceLock) == LCEVC_Success;
        for (uint32_t p = 0; sourceOk && p < srcPlanes; ++p) {
            sourceOk = LCEVC_GetPictureLockPlaneDesc(ctx->lcevc, sourceLock, p, &src[p]) == LCEVC_Success &&
                       src[p].firstSample;
        }
        if (!sourceOk && sourceLock.hdl) {
            LCEVC_UnlockPicture(ctx->lcevc, sourceLock);
        }
    }
    if (!sourceOk) {
        AHardwareBuffer_unlock(buffer, nullptr);
        ctx->presenter->recycleBuffer(buffer);
        return false;
    }

    auto* dstY = static_cast<uint8_t*>(planes.planes[0].data);
    auto* const dstChroma = std::min(cbPtr, crPtr);
    const size_t dstYStride = planes.planes[0].rowStride;
    const size_t dstChromaStride = planes.planes[1].rowStride;
    if (hold->format == LCEVC_I420_10_LE) {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.convert10");
        const vcat::yuv::Plane10 y{src[0].firstSample, src[0].rowByteStride};
        const vcat::yuv::Plane10 u{src[1].firstSample, src[1].rowByteStride};
        const vcat::yuv::Plane10 v{src[2].firstSample, src[2].rowByteStride};
        if (useP010) {
            vcat::yuv::convertI420P10ToP010(*ctx->convertPool, y, u, v, dstY, dstYStride, dstChroma,
                                            dstChromaStride, hold->width, hold->height, swapCbCr);
        } else {
            vcat::yuv::convertI420P10ToSemiPlanar8(*ctx->convertPool, y, u, v, dstY, dstYStride,
                                                   dstChroma, dstChromaStride, hold->width,
                                                   hold->height, swapCbCr);
        }
    } else {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.copy8");
        for (uint32_t y = 0; y < hold->height; ++y) {
            std::memcpy(dstY + y * dstYStride, src[0].firstSample + y * src[0].rowByteStride, hold->width);
        }
        copyChroma8(src, srcSemiPlanar, dstChroma, dstChromaStride, (hold->width + 1) / 2,
                    (hold->height + 1) / 2, swapCbCr);
    }

    {
        TRACE_SCOPE("jni:vcat.lcevc.copyToPresenterBuffer.unlock");
        LCEVC_UnlockPicture(ctx->lcevc, sourceLock);
        AHardwareBuffer_unlock(buffer, nullptr);
    }

    ctx->outputPicturePool.release(ctx->lcevc, hold->pic, hold->width, hold->height, hold->format);
    hold->pic = {};
    hold->buffer = buffer;
    hold->presenter = ctx->presenter;
    return true;
}

// True when the next frame Java will dequeue is already in the ready queue.
static bool nextOutputReady(const NativeCtx* ctx)
{
    return !ctx->expected.empty() &&
           std::any_of(ctx->ready.begin(), ctx->ready.end(),
                       [ctx](const OutputHolder* h) { return h->pts == ctx->expected.front(); });
}

static void lcevcDrainOutputs(NativeCtx* ctx)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.lcevc.drainOutputs");
    while (true) {
        // Every drained output holds a pool buffer: when the decoder runs ahead of display,
        // leave completed pictures in LCEVCdec rather than exhaust the pool.
        if (ctx->ready.size() >= kMaxReadyOutputs && nextOutputReady(ctx)) {
            break;
        }
        LCEVC_PictureHandle out{};
        LCEVC_DecodeInformation info{};
        LCEVC_ReturnCode rc = LCEVC_ReceiveDecoderPicture(ctx->lcevc, &out, &info);
        if (rc == LCEVC_Again) {
            break;
        }
        if (rc != LCEVC_Success) {
            if (rc != LCEVC_Flushed) {
                LOGW("ReceiveDecoderPicture: %s", lcevcError(rc));
            }
            break;
        }
        LCEVC_PictureDesc desc{};
        uint32_t w = info.baseWidth;
        uint32_t h = info.baseHeight;
        if (LCEVC_GetPictureDesc(ctx->lcevc, out, &desc) == LCEVC_Success) {
            w = desc.width;
            h = desc.height;
        }
        auto* hold = new (std::nothrow) OutputHolder();
        if (!hold) {
            freeReceivedPicture(ctx, out);
            break;
        }
        hold->generation = ctx->generation;
        hold->lcevc = ctx->lcevc;
        hold->pic = out;
        hold->width = w;
        hold->height = h;
        hold->format = desc.colorFormat;
        hold->pts = static_cast<int64_t>(info.timestamp);
        auto external = ctx->externalOutputs.find(out.hdl);
        if (external != ctx->externalOutputs.end()) {
            hold->buffer = external->second;
            hold->presenter = ctx->presenter;
            ctx->externalOutputs.erase(external);
            if (!info.enhanced) {
                hold->width = info.baseWidth;
                hold->height = info.baseHeight;
            }
            // The CPU pipeline unlocks the buffer and finishes writing it as the picture is freed.
            // Vulkan writes are complete once received, and the picture stays bound for reuse.
            if (!ctx->boundPictures.count(hold->buffer)) {
                LCEVC_FreePicture(ctx->lcevc, out);
            }
            hold->pic = {};
        } else {
            // Copy here rather than on the render thread; without a window yet, presentation
            // retries the copy.
            copyToPresenterBuffer(ctx, hold);
        }
        ctx->ready.push_back(hold);
    }
}

// Retain an unsent base across LCEVC backpressure instead of dropping it.
static void lcevcProcessBase(NativeCtx* ctx, vvdecFrame* frame)
{
    auto& job = ctx->baseJob;
    job = {};
    job.pts = frame->cts;
    bool copied = lcevcCopyBase(ctx, frame, &job.pic);
    {
        std::lock_guard<std::mutex> vvdec(ctx->vvdecMutex);
        vvdec_frame_unref(ctx->dec, frame);
    }
    if (!copied) {
        ctx->error = -EIO;
        return;
    }
    ctx->haveBaseJob = true;
}

static void releaseAllPending(NativeCtx* ctx)
{
    if (ctx->haveBaseJob && !ctx->baseJob.baseSent) {
        LCEVC_PictureDesc desc{};
        if (LCEVC_GetPictureDesc(ctx->lcevc, ctx->baseJob.pic, &desc) == LCEVC_Success) {
            ctx->basePicturePool.release(ctx->lcevc, ctx->baseJob.pic, desc.width, desc.height,
                                         desc.colorFormat);
        } else {
            LCEVC_FreePicture(ctx->lcevc, ctx->baseJob.pic);
        }
    }
    ctx->haveBaseJob = false;
}

static void releaseAllReady(NativeCtx* ctx)
{
    while (!ctx->ready.empty()) {
        OutputHolder* h = ctx->ready.front();
        ctx->ready.pop_front();
        if (h) {
            // Completed buffers/pictures belong to the plugin until released.
            if (h->buffer) {
                h->presenter->recycleBuffer(h->buffer);
            } else {
                ctx->outputPicturePool.release(ctx->lcevc, h->pic, h->width, h->height, h->format);
            }
            delete h;
        }
    }
}

// Output completion is polled independently of compressed input arrival.
static void pump(NativeCtx* ctx)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.pump");
    lcevcRecycleBases(ctx);
    lcevcDrainOutputs(ctx);
    if (ctx->error) {
        return;
    }
    if (ctx->haveBaseJob) {
        auto& job = ctx->baseJob;
        if (!job.baseSent) {
            auto rc = LCEVC_SendDecoderBase(ctx->lcevc, job.pts, job.pic, UINT32_MAX, nullptr);
            if (rc == LCEVC_Again) {
                return;
            }
            if (rc != LCEVC_Success) {
                ctx->error = -EIO;
                return;
            }
            job.baseSent = true;
        }
        auto rc = lcevcSendOutputPicture(ctx, job.pts);
        if (rc == LCEVC_Again) {
            return;
        }
        if (rc != LCEVC_Success) {
            ctx->error = -EIO;
            return;
        }
        ctx->expected.push_back(job.pts);
        ctx->haveBaseJob = false;
    }
    if (ctx->eos && !ctx->baseDrained && ctx->expected.size() < kMaxPendingPackets) {
        vvdecFrame* out = nullptr;
        int rc;
        {
            std::lock_guard<std::mutex> vvdec(ctx->vvdecMutex);
            rc = vvdec_flush(ctx->dec, &out);
        }
        if (out) {
            lcevcProcessBase(ctx, out);
        }
        if (rc != VVDEC_OK && rc != VVDEC_EOF && rc != VVDEC_TRY_AGAIN) {
            ctx->error = -EIO;
        }
        if (!out || rc == VVDEC_EOF) {
            ctx->baseDrained = true;
        }
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*)
{
#if VN_SDK_FEATURE(TRACING_SYSTEM_SINK)
    ldcDiagnosticsInitialize(nullptr);
    if (!lcevcPerfettoSdkRegister()) {
        LOGE("JNI_OnLoad: could not register Perfetto diagnostics handler");
    }
#endif
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM*, void*)
{
#if VN_SDK_FEATURE(TRACING_SYSTEM_SINK)
    lcevcPerfettoSdkUnregister();
    ldcDiagnosticsRelease();
#endif
}

extern "C" JNIEXPORT jlong JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeCreate(
    JNIEnv*, jclass, jint threads, jint lcevcThreads, jint gpuPipeline)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeCreate");
    auto* ctx = new (std::nothrow) NativeCtx();
    if (!ctx) {
        LOGE("nativeCreate: failed to allocate NativeCtx");
        return 0;
    }
    ctx->basePicturePool.maxCount = 3;
    ctx->outputPicturePool.maxCount = 6;

    vvdecParams p;
    vvdec_params_default(&p);
    p.threads = (threads == 0) ? 1 : threads;

    ctx->params = p;
    ctx->dec = vvdec_decoder_open(&p);
    if (!ctx->dec) {
        LOGE("nativeCreate: vvdec_decoder_open failed (threads=%d)", p.threads);
        delete ctx;
        return 0;
    }

    ctx->lcevcData = static_cast<uint8_t*>(std::malloc(kEnhancementBufBytes));
    if (!ctx->lcevcData) {
        LOGE("nativeCreate: failed to allocate lcevc data buffer");
        vvdec_decoder_close(ctx->dec);
        delete ctx;
        return 0;
    }

    LCEVC_AccelContextHandle accel{};
    accel.hdl = 0;
    LCEVC_ReturnCode rc = LCEVC_CreateDecoder(&ctx->lcevc, accel);
    if (rc != LCEVC_Success) {
        LOGE("nativeCreate: LCEVC_CreateDecoder: %s", lcevcError(rc));
        std::free(ctx->lcevcData);
        vvdec_decoder_close(ctx->dec);
        delete ctx;
        return 0;
    }

    // Route the public LCEVC_Log event to logcat. Register before the remaining
    // configuration so decoder setup diagnostics are captured as well.
    {
        LCEVC_ReturnCode lrc = LCEVC_SetDecoderEventCallback(ctx->lcevc, lcevcEventCallback, ctx);
        if (lrc == LCEVC_Success) {
            const int32_t events[] = {static_cast<int32_t>(LCEVC_Log)};
            lrc = LCEVC_ConfigureDecoderIntArray(ctx->lcevc, "events", 1, events);
        }
        if (lrc != LCEVC_Success) {
            LOGE("nativeCreate: enable LCEVC_Log events: %s", lcevcError(lrc));
            LCEVC_DestroyDecoder(ctx->lcevc);
            std::free(ctx->lcevcData);
            vvdec_decoder_close(ctx->dec);
            delete ctx;
            return 0;
        }

        // Warning level keeps failure diagnostics while staying off the per-frame
        // debug path.
        lrc = LCEVC_ConfigureDecoderInt(ctx->lcevc, "log_level", static_cast<int32_t>(LCEVC_LogWarning));
        if (lrc != LCEVC_Success) {
            LOGW("nativeCreate: log_level config: %s", lcevcError(lrc));
        }
    }

    // Select the LCEVC pipeline
    ctx->vulkanPipeline = (gpuPipeline != 0);
    rc = LCEVC_ConfigureDecoderString(ctx->lcevc, "pipeline", ctx->vulkanPipeline ? "vulkan" : "cpu");
    if (rc != LCEVC_Success) {
        LCEVC_DestroyDecoder(ctx->lcevc);
        std::free(ctx->lcevcData);
        vvdec_decoder_close(ctx->dec);
        delete ctx;
        return 0;
    }
    // ReceiveDecoderPicture waits for an incomplete oldest frame when the number
    // of processing frames exceeds min_latency. Match that threshold to this
    // wrapper's own bound so polling never sleeps while more input can be decoded.
    LCEVC_ReturnCode mrc =
        LCEVC_ConfigureDecoderInt(ctx->lcevc, "min_latency", static_cast<int32_t>(kMaxPendingPackets));
    if (mrc != LCEVC_Success) {
        LOGW("nativeCreate: min_latency config: %s", lcevcError(mrc));
    }

    if (lcevcThreads > 0) {
        LCEVC_ReturnCode crc =
            LCEVC_ConfigureDecoderInt(ctx->lcevc, "threads", static_cast<int32_t>(lcevcThreads));
        if (crc != LCEVC_Success) {
            // Non-fatal: log and continue with the LCEVCdec default.
            LOGW("nativeCreate: LCEVC_ConfigureDecoderInt(\"threads\",%d) failed: %s -- continuing "
                 "with default",
                 static_cast<int>(lcevcThreads), lcevcError(crc));
        } else {
            LOGI("nativeCreate: LCEVC threads configured = %d", static_cast<int>(lcevcThreads));
        }
    } else {
        LOGI("nativeCreate: LCEVC threads = auto (LCEVCdec Android default)");
    }

    // An automatic decoder thread count uses one conversion slice.
    const uint32_t convertThreads = lcevcThreads > 0 ? static_cast<uint32_t>(lcevcThreads) : 1;
    ctx->convertPool = std::make_unique<SliceThreadPool>(convertThreads);

#if VN_SDK_FEATURE(TRACING_SYSTEM_SINK)
    if (ctx->vulkanPipeline) {
        LCEVC_ReturnCode timestampRc = LCEVC_ConfigureDecoderInt(ctx->lcevc, "vulkan_timestamps", 256);
        if (timestampRc != LCEVC_Success) {
            LOGW("nativeCreate: enabling Vulkan timestamps failed: %s", lcevcError(timestampRc));
        }
    }
#endif

    rc = LCEVC_InitializeDecoder(ctx->lcevc);
    if (rc != LCEVC_Success) {
        LOGE("nativeCreate: LCEVC_InitializeDecoder: %s", lcevcError(rc));
        LCEVC_DestroyDecoder(ctx->lcevc);
        std::free(ctx->lcevcData);
        vvdec_decoder_close(ctx->dec);
        delete ctx;
        return 0;
    }

    ctx->presenter = std::make_shared<SurfacePresenter>();

    LOGI("Created vvdec=%s threads=%d, LCEVC pipeline=%s threads=%d", vvdec_get_version(),
         p.threads, ctx->vulkanPipeline ? "vulkan" : "cpu", lcevcThreads);
    return reinterpret_cast<jlong>(ctx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeFlush(JNIEnv*, jclass, jlong handle)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeFlush");
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx) {
        return;
    }

    std::unique_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
    std::lock_guard<std::mutex> vvdec(ctx->vvdecMutex);
    if (!ctx->dec) {
        return;
    }

    ++ctx->generation;
    ctx->expected.clear();
    ctx->decodeOnly.clear();
    ctx->error = 0;
    releaseAllPending(ctx);
    releaseAllReady(ctx);

    LCEVC_FlushDecoder(ctx->lcevc);
    lcevcRecycleBases(ctx);
    while (true) {
        LCEVC_PictureHandle out{};
        LCEVC_DecodeInformation info{};
        LCEVC_ReturnCode rc = LCEVC_ReceiveDecoderPicture(ctx->lcevc, &out, &info);
        if (rc == LCEVC_Again) {
            break;
        }
        if (rc != LCEVC_Success && rc != LCEVC_Flushed) {
            break;
        }
        freeReceivedPicture(ctx, out);
    }

    for (auto& entry : ctx->externalOutputs) {
        ctx->presenter->recycleBuffer(entry.second);
    }
    ctx->externalOutputs.clear();
    ctx->presenter->setWindow(nullptr);
    ctx->presenter->setWindow(ctx->win);
    // Drain any remaining base frames from vvdec.
    vvdecFrame* f = nullptr;
    while (true) {
        int ret = vvdec_flush(ctx->dec, &f);
        if (f) {
            vvdec_frame_unref(ctx->dec, f);
            f = nullptr;
        }
        if (ret != VVDEC_OK) {
            break;
        }
    }
    lcevcRecycleBases(ctx);

    vvdec_decoder_close(ctx->dec);
    ctx->dec = vvdec_decoder_open(&ctx->params);
    if (!ctx->dec) {
        ctx->error = -EIO;
    }
    ctx->baseDrained = false;
    ctx->eos = false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeClose(JNIEnv*, jclass, jlong handle)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeClose");
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx) {
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
        std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
        std::lock_guard<std::mutex> vvdec(ctx->vvdecMutex);
        releaseAllPending(ctx);
        releaseAllReady(ctx);
        // Any output pictures still with the decoder are freed by LCEVC_DestroyDecoder.

        lcevcRecycleBases(ctx);
        // Free pooled pictures before their owning decoder.
        ctx->basePicturePool.releaseAll(ctx->lcevc);
        ctx->outputPicturePool.releaseAll(ctx->lcevc);
        for (auto& entry : ctx->boundPictures) {
            if (!ctx->externalOutputs.count(entry.second.hdl)) {
                LCEVC_FreePicture(ctx->lcevc, entry.second);
            }
        }
        ctx->boundPictures.clear();
        LCEVC_DestroyDecoder(ctx->lcevc);
        for (auto& entry : ctx->externalOutputs) {
            ctx->presenter->recycleBuffer(entry.second);
        }
        ctx->externalOutputs.clear();
        if (ctx->dec) {
            vvdec_decoder_close(ctx->dec);
            ctx->dec = nullptr;
        }
        if (ctx->lcevcData) {
            std::free(ctx->lcevcData);
            ctx->lcevcData = nullptr;
        }
    }

    if (ctx->win) {
        ANativeWindow_release(ctx->win);
    }

    if (ctx->presenter) {
        ctx->presenter->setWindow(nullptr);
    }
    delete ctx;
}

extern "C" JNIEXPORT jint JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeQueueInput(
    JNIEnv* env, jclass, jlong handle, jobject byteBuffer, jint offset, jint size, jlong ptsUs,
    jboolean decodeOnly)
{
    LCEVC_JNI_TRACE_SCOPE_ARGS("jni:vcat.nativeQueueInput", "timestamp", ptsUs, "bytes", size);
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx || !byteBuffer || size <= 0) {
        return -EINVAL;
    }
    std::shared_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    const jlong cap = env->GetDirectBufferCapacity(byteBuffer);
    if (cap < 0 || offset < 0 || static_cast<jlong>(offset) + size > cap) {
        return -EINVAL;
    }
    auto* src = static_cast<uint8_t*>(env->GetDirectBufferAddress(byteBuffer));
    if (!src) {
        return -EINVAL;
    }
    src += offset;

    {
        std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
        pump(ctx);
        if (ctx->error) {
            return ctx->error;
        }
        if (ctx->haveBaseJob || ctx->expected.size() >= kMaxPendingPackets) {
            return -EAGAIN;
        }
        uint32_t enhSize = 0;
        int found = LCEVC_extractEnhancementFromNAL(src, size, detectNalFormat(src, size),
                                                    LCEVC_CodecType_H266, ctx->lcevcData,
                                                    kEnhancementBufBytes, &enhSize);
        if (found < 0) {
            return -EINVAL;
        }
        if (found > 0 && enhSize > 0) {
            auto rc = LCEVC_SendDecoderEnhancementData(ctx->lcevc, ptsUs, ctx->lcevcData, enhSize);
            if (rc == LCEVC_Again) {
                return -EAGAIN; // Nothing has consumed this input yet.
            }
            if (rc != LCEVC_Success) {
                return -EIO;
            }
        }
        ctx->decodeOnly[ptsUs] = decodeOnly;
    }

    vvdecAccessUnit au{};
    vvdec_accessUnit_default(&au);
    au.payload = src;
    au.payloadSize = au.payloadUsedSize = size;
    au.cts = ptsUs;
    au.ctsValid = true;
    vvdecFrame* out = nullptr;
    int rc;
    {
        std::lock_guard<std::mutex> vvdec(ctx->vvdecMutex);
        if (!ctx->dec) {
            return -EINVAL;
        }
        rc = vvdec_decode(ctx->dec, &au, &out);
        au.payload = nullptr; // The direct buffer is borrowed only for this call.
        if (rc != VVDEC_OK && rc != VVDEC_TRY_AGAIN && rc != VVDEC_EOF && out) {
            vvdec_frame_unref(ctx->dec, out);
            out = nullptr;
        }
    }

    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
    if (rc != VVDEC_OK && rc != VVDEC_TRY_AGAIN && rc != VVDEC_EOF) {
        ctx->error = -EIO;
        ctx->decodeOnly.erase(ptsUs);
        return ctx->error;
    }
    if (out) {
        lcevcProcessBase(ctx, out);
    }
    pump(ctx);
    return ctx->error;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeDequeueFrame(
    JNIEnv* env, jclass, jlong handle, jintArray outWH, jlongArray outPtsUs)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeDequeueFrame");

    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx) {
        return 0;
    }
    if (!outWH || !outPtsUs) {
        return 0;
    }

    std::shared_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
    if (!ctx->dec) {
        return 0;
    }

    pump(ctx);

    jint empty[3] = {0, 0, 0};
    if (ctx->error) {
        empty[0] = kDequeueError;
        empty[1] = ctx->error;
    } else if (ctx->eos && ctx->baseDrained && !ctx->haveBaseJob && ctx->expected.empty()) {
        empty[0] = kDequeueEndOfStream;
    }
    env->SetIntArrayRegion(outWH, 0, 3, empty);
    if (ctx->error || ctx->expected.empty()) {
        return 0;
    }
    // A later completion must not overtake a base frame submitted earlier.
    auto at = ctx->ready.begin();
    while (at != ctx->ready.end() && (*at)->pts != ctx->expected.front()) {
        ++at;
    }
    if (at == ctx->ready.end()) {
        return 0;
    }
    OutputHolder* hold = *at;
    ctx->ready.erase(at);
    ctx->expected.pop_front();

    jint wh[3] = {static_cast<jint>(hold->width), static_cast<jint>(hold->height),
                  ctx->decodeOnly[hold->pts] ? 1 : 0};
    ctx->decodeOnly.erase(hold->pts);
    env->SetIntArrayRegion(outWH, 0, 3, wh);

    jlong pts[1] = {static_cast<jlong>(hold->pts)};
    env->SetLongArrayRegion(outPtsUs, 0, 1, pts);

    return reinterpret_cast<jlong>(hold);
}

extern "C" JNIEXPORT void JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeSetSurface(
    JNIEnv* env, jclass, jlong handle, jobject surface)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeSetSurface");

    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx) {
        return;
    }

    std::shared_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);

    if (ctx->win) {
        ANativeWindow_release(ctx->win);
        ctx->win = nullptr;
    }
    if (surface) {
        ctx->win = ANativeWindow_fromSurface(env, surface);
    }

    // The presenter retains its own window reference.
    if (!ctx->presenter->setWindow(ctx->win)) {
        LOGE("nativeSetSurface: cannot present to this surface");
    }
}

extern "C" JNIEXPORT jint JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeRenderToSurface(
    JNIEnv*, jclass, jlong handle, jlong nativePic)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeRenderToSurface");
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    auto* hold = reinterpret_cast<OutputHolder*>(nativePic);
    if (!ctx || !hold) {
        return -EINVAL;
    }
    std::shared_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
    if (hold->generation != ctx->generation) {
        return 0;
    }
    if (!hold->buffer && !copyToPresenterBuffer(ctx, hold)) {
        return -ENODEV;
    }
    auto* buffer = hold->buffer;
    hold->buffer = nullptr;
    bool presented = hold->presenter->presentBuffer(buffer, hold->width, hold->height);
    return presented ? 0 : -ENODEV;
}

extern "C" JNIEXPORT void JNICALL Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeReleasePicture(
    JNIEnv*, jclass, jlong handle, jlong nativePic)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeReleasePicture");
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    auto* hold = reinterpret_cast<OutputHolder*>(nativePic);
    if (!hold) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle;
    if (ctx) {
        lifecycle = std::shared_lock<std::shared_mutex>(ctx->lifecycleMutex);
    }
    if (hold->buffer) {
        hold->presenter->recycleBuffer(hold->buffer);
    } else if (hold->pic.hdl && ctx) {
        std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
        ctx->outputPicturePool.release(ctx->lcevc, hold->pic, hold->width, hold->height, hold->format);
    }
    delete hold;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_vvdecGetVersion(JNIEnv* env, jclass)
{
    const char* v = vvdec_get_version();
    return env->NewStringUTF(v ? v : "unknown");
}

extern "C" JNIEXPORT void JNICALL
Java_com_roncatech_libvcat_lcevcvvdec_NativeLcevcVvdec_nativeSignalEof(JNIEnv*, jclass, jlong handle)
{
    LCEVC_JNI_TRACE_SCOPE("jni:vcat.nativeSignalEof");
    auto* ctx = reinterpret_cast<NativeCtx*>(handle);
    if (!ctx) {
        return;
    }
    std::shared_lock<std::shared_mutex> lifecycle(ctx->lifecycleMutex);
    std::lock_guard<std::mutex> lcevc(ctx->lcevcMutex);
    ctx->eos = true;
}
