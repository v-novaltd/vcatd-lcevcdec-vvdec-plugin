/* Copyright (c) V-Nova International Limited 2026. All rights reserved.
 * This software is licensed under the BSD-3-Clause-Clear License by V-Nova Limited.
 * No patent licenses are granted under this license. For enquiries about patent licenses,
 * please contact legal@v-nova.com.
 * The LCEVCdec software is a stand-alone project and is NOT A CONTRIBUTION to any other project.
 * If the software is incorporated into another project, THE TERMS OF THE BSD-3-CLAUSE-CLEAR LICENSE
 * AND THE ADDITIONAL LICENSING INFORMATION CONTAINED IN THIS FILE MUST BE MAINTAINED, AND THE
 * SOFTWARE DOES NOT AND MUST NOT ADOPT THE LICENSE OF THE INCORPORATING PROJECT. However, the
 * software may be incorporated into a project under a compatible license provided the requirements
 * of the BSD-3-Clause-Clear license are respected, and V-Nova Limited remains
 * licensor of the software ONLY UNDER the BSD-3-Clause-Clear license (not the compatible license).
 * ANY ONWARD DISTRIBUTION, WHETHER STAND-ALONE OR AS PART OF ANY OTHER PROJECT, REMAINS SUBJECT TO
 * THE EXCLUSION OF PATENT LICENSES PROVISION OF THE BSD-3-CLAUSE-CLEAR LICENSE. */

#include "surface_presenter.h"

#include <android/log.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

constexpr const char* PRESENTER_TAG = "lcevc_dec_presenter";

#define PLOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, PRESENTER_TAG, __VA_ARGS__))
#define PLOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, PRESENTER_TAG, __VA_ARGS__))
#define PLOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, PRESENTER_TAG, __VA_ARGS__))
#define PLOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, PRESENTER_TAG, __VA_ARGS__))

// Enough buffers for the client's decode pipeline: up to 3 concurrent decodes,
// 6 queued decoded frames, one on screen and one in flight. Large RGBA frames are
// limited by the memory budget instead, down to a minimum that still keeps the
// decoder, the queued output and the display busy.
static constexpr uint32_t kMaxBuffers = 12;
static constexpr uint32_t kMinBuffers = 6;
static constexpr uint64_t kBudgetBytes = 256ull * 1024 * 1024;

// HAL_PIXEL_FORMAT_YCrCb_420_SP (NV21), which the NDK AHardwareBuffer enum lacks
static constexpr uint32_t kFormatYCrCb420SP = 0x11;

// Approximate memory size of a buffer, for the pool budget.
static uint64_t bufferBytes(const AHardwareBuffer_Desc& desc)
{
    const uint64_t pixels = static_cast<uint64_t>(desc.width) * desc.height;
    switch (desc.format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM: return pixels * 4;
        case AHARDWAREBUFFER_FORMAT_YCbCr_P010: return pixels * 3;
        default: return pixels * 3 / 2;
    }
}

SurfacePresenter::~SurfacePresenter()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    destroySurfaceControlLocked(lock);
    releaseBuffersLocked();
    if (m_presentedBuffer != nullptr) {
        AHardwareBuffer_release(m_presentedBuffer);
        m_presentedBuffer = nullptr;
    }
    if (m_window != nullptr) {
        ANativeWindow_release(m_window);
        m_window = nullptr;
    }
}

bool SurfacePresenter::setWindow(ANativeWindow* window)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (window == m_window) {
        return true;
    }

    destroySurfaceControlLocked(lock);

    if (m_window != nullptr) {
        ANativeWindow_release(m_window);
        m_window = nullptr;
    }

    if (window == nullptr) {
        return true;
    }

    ANativeWindow_acquire(window);
    m_window = window;
    m_surfaceControl = ASurfaceControl_createFromWindow(window, "LcevcVideo");
    if (m_surfaceControl == nullptr) {
        PLOGE("setWindow: ASurfaceControl_createFromWindow failed");
        ANativeWindow_release(m_window);
        m_window = nullptr;
        return false;
    }

    m_layerOwner = std::shared_ptr<ASurfaceControl>(m_surfaceControl, ASurfaceControl_release);

    // Force geometry to be reapplied on the next present
    m_lastFrameWidth = 0;
    m_lastFrameHeight = 0;

    PLOGI("setWindow: attached to window %p", static_cast<void*>(window));
    return true;
}

bool SurfacePresenter::ready()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_surfaceControl != nullptr;
}

// Choose the CPU-written buffer usage flags. Prefer COMPOSER_OVERLAY (lets
// SurfaceFlinger scan the buffer out directly), but some devices (e.g. Pixel 4a)
// return an opaque, non-CPU-lockable plane layout for overlay-usage YUV buffers -
// probe once and fall back to GPU composition there.
uint64_t SurfacePresenter::chooseBufferUsageLocked()
{
    const uint64_t base = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    const uint64_t withOverlay = base | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    if (!probeLockableLocked(AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, withOverlay, nullptr)) {
        PLOGI("chooseBufferUsage: overlay-usage buffers are not CPU-lockable on this device, "
              "using GPU composition");
        return base;
    }
    return withOverlay;
}

// Allocate a small buffer and check it locks as a usable 3-plane YUV layout.
// `crFirst` (optional) reports whether Cr precedes Cb in memory.
bool SurfacePresenter::probeLockableLocked(uint32_t format, uint64_t usage, bool* crFirst)
{
    const AHardwareBuffer_Desc probeDesc = {
        .width = 64,
        .height = 64,
        .layers = 1,
        .format = format,
        .usage = usage,
    };

    AHardwareBuffer* probe = nullptr;
    if (AHardwareBuffer_allocate(&probeDesc, &probe) != 0) {
        return false;
    }

    AHardwareBuffer_Planes planes{};
    bool lockable = AHardwareBuffer_lockPlanes(probe, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1,
                                               nullptr, &planes) == 0;
    if (lockable) {
        lockable = planes.planeCount >= 3 && planes.planes[0].rowStride != 0 &&
                   planes.planes[1].pixelStride != 0;
        for (uint32_t p = 0; p < 3 && lockable; ++p) {
            lockable = planes.planes[p].data && planes.planes[p].rowStride && planes.planes[p].pixelStride;
        }
        if (lockable && crFirst) {
            *crFirst = planes.planes[2].data < planes.planes[1].data;
        }
        AHardwareBuffer_unlock(probe, nullptr);
    }
    AHardwareBuffer_release(probe);
    return lockable;
}

// Some MediaTek composers scan out CPU-written flexible YUV buffers without their chroma or crop,
// although gralloc and the GPU agree on an NV21 layout. The explicit NV21 HAL format has the same
// memory layout and displays correctly, so use it where flexible YUV locks as NV21 and buffers
// are scanned out directly. GPU-composited buffers keep flexible YUV, which works there.
uint64_t SurfacePresenter::cpuBufferUsage()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cpuBufferUsage == 0) {
        m_cpuBufferUsage = chooseBufferUsageLocked();
    }
    return m_cpuBufferUsage;
}

uint32_t SurfacePresenter::cpuYuv420Format()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cpuYuv420Format != 0) {
        return m_cpuYuv420Format;
    }
    if (m_cpuBufferUsage == 0) {
        m_cpuBufferUsage = chooseBufferUsageLocked();
    }

    m_cpuYuv420Format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
    if ((m_cpuBufferUsage & AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY) == 0) {
        return m_cpuYuv420Format;
    }
    bool flexibleCrFirst = false;
    bool explicitCrFirst = false;
    if (probeLockableLocked(AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420, m_cpuBufferUsage, &flexibleCrFirst) &&
        flexibleCrFirst && probeLockableLocked(kFormatYCrCb420SP, m_cpuBufferUsage, &explicitCrFirst) &&
        explicitCrFirst) {
        PLOGI("cpuYuv420Format: flexible YUV is NV21, using explicit NV21 buffers");
        m_cpuYuv420Format = kFormatYCrCb420SP;
    }
    return m_cpuYuv420Format;
}

void SurfacePresenter::disableExplicitNv21()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cpuYuv420Format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
}

AHardwareBuffer* SurfacePresenter::allocateBufferLocked(uint32_t width, uint32_t height,
                                                        uint32_t format, uint64_t usage)
{
    const AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = usage,
    };

    AHardwareBuffer* buffer = nullptr;
    if (const int ret = AHardwareBuffer_allocate(&desc, &buffer); ret != 0) {
        PLOGE("allocateBuffer: AHardwareBuffer_allocate %ux%u format=0x%x usage=0x%llx failed: %d",
              width, height, format, static_cast<unsigned long long>(usage), ret);
        return nullptr;
    }
    m_allocatedCount++;
    m_allocatedBytes += bufferBytes(desc);
    PLOGD("allocateBuffer: %ux%u format=0x%x usage=0x%llx (%u allocated)", width, height, format,
          static_cast<unsigned long long>(usage), m_allocatedCount);
    return buffer;
}

AHardwareBuffer* SurfacePresenter::acquireBuffer(uint32_t width, uint32_t height, uint32_t format,
                                                 uint64_t usage, bool* exhausted, AHardwareBuffer** evicted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (exhausted) {
        *exhausted = false;
    }
    if (evicted) {
        *evicted = nullptr;
    }

    // Reuse a free buffer of matching size and format
    for (auto it = m_freeBuffers.begin(); it != m_freeBuffers.end(); ++it) {
        AHardwareBuffer_Desc desc{};
        if (it->fence >= 0) {
            pollfd fd{it->fence, POLLIN, 0};
            if (poll(&fd, 1, 0) <= 0) {
                continue;
            }
            close(it->fence);
            it->fence = -1;
        }
        AHardwareBuffer_describe(it->buffer, &desc);
        if (desc.width == width && desc.height == height && desc.format == format && desc.usage == usage) {
            AHardwareBuffer* buffer = it->buffer;
            m_freeBuffers.erase(it);
            return buffer;
        }
    }

    // Discard one stale free buffer per acquire to let the pool adapt
    if (!m_freeBuffers.empty()) {
        AHardwareBuffer_Desc stale{};
        AHardwareBuffer_describe(m_freeBuffers.back().buffer, &stale);
        if (m_freeBuffers.back().fence >= 0) {
            close(m_freeBuffers.back().fence);
        }
        AHardwareBuffer_release(m_freeBuffers.back().buffer);
        if (evicted) {
            *evicted = m_freeBuffers.back().buffer;
        }
        m_freeBuffers.pop_back();
        m_allocatedCount--;
        m_allocatedBytes -= std::min(m_allocatedBytes, bufferBytes(stale));
    }

    const AHardwareBuffer_Desc wanted = {.width = width, .height = height, .format = format};
    const uint64_t bytes = bufferBytes(wanted);
    if (m_allocatedCount >= kMaxBuffers ||
        (m_allocatedCount >= kMinBuffers && m_allocatedBytes + bytes > kBudgetBytes)) {
        if (exhausted) {
            *exhausted = true;
        }
        return nullptr;
    }

    return allocateBufferLocked(width, height, format, usage);
}

void SurfacePresenter::recycleBuffer(AHardwareBuffer* buffer)
{
    if (buffer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_freeBuffers.push_back({buffer});
}

// Called when a transaction's updates are on screen - the previously presented
// buffer is no longer scanned out and can be reused.
void SurfacePresenter::onTransactionComplete(void* context, ASurfaceTransactionStats* stats)
{
    auto* presentContext = static_cast<PresentContext*>(context);
    auto self = presentContext->self;
    if (presentContext->previous) {
        int fence =
            ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, presentContext->layer.get());
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_freeBuffers.push_back({presentContext->previous, fence});
    }
    delete presentContext;
}

void SurfacePresenter::applyGeometryLocked(ASurfaceTransaction* transaction, uint32_t frameWidth,
                                           uint32_t frameHeight)
{
    const auto windowWidth = static_cast<uint32_t>(ANativeWindow_getWidth(m_window));
    const auto windowHeight = static_cast<uint32_t>(ANativeWindow_getHeight(m_window));

    if (frameWidth == m_lastFrameWidth && frameHeight == m_lastFrameHeight &&
        windowWidth == m_lastWindowWidth && windowHeight == m_lastWindowHeight) {
        return;
    }
    if (frameWidth == 0 || frameHeight == 0 || windowWidth == 0 || windowHeight == 0) {
        return;
    }

    m_lastFrameWidth = frameWidth;
    m_lastFrameHeight = frameHeight;
    m_lastWindowWidth = windowWidth;
    m_lastWindowHeight = windowHeight;

    // Scale to fit the window, centred (letterbox/pillarbox)
    const float scaleX = static_cast<float>(windowWidth) / static_cast<float>(frameWidth);
    const float scaleY = static_cast<float>(windowHeight) / static_cast<float>(frameHeight);
    const float scale = std::min(scaleX, scaleY);

    const auto drawWidth = static_cast<int32_t>(static_cast<float>(frameWidth) * scale);
    const auto drawHeight = static_cast<int32_t>(static_cast<float>(frameHeight) * scale);
    const int32_t left = (static_cast<int32_t>(windowWidth) - drawWidth) / 2;
    const int32_t top = (static_cast<int32_t>(windowHeight) - drawHeight) / 2;

    const ARect source = {0, 0, static_cast<int32_t>(frameWidth), static_cast<int32_t>(frameHeight)};
    const ARect destination = {left, top, left + drawWidth, top + drawHeight};
    ASurfaceTransaction_setGeometry(transaction, m_surfaceControl, source, destination,
                                    /*transform=*/0);

    PLOGI("applyGeometry: frame %ux%u window %ux%u scale %.3f dst (%d,%d %dx%d)", frameWidth,
          frameHeight, windowWidth, windowHeight, scale, left, top, drawWidth, drawHeight);
}

bool SurfacePresenter::presentBuffer(AHardwareBuffer* buffer, uint32_t frameWidth, uint32_t frameHeight)
{
    if (buffer == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_surfaceControl == nullptr) {
        PLOGW("presentBuffer: no surface control - dropping frame");
        m_freeBuffers.push_back({buffer});
        return false;
    }

    auto* presentContext = new PresentContext{shared_from_this(), m_presentedBuffer, m_layerOwner};
    m_presentedBuffer = buffer;

    ASurfaceTransaction* transaction = ASurfaceTransaction_create();
    ASurfaceTransaction_setBuffer(transaction, m_surfaceControl, buffer, /*acquire_fence_fd=*/-1);
    applyGeometryLocked(transaction, frameWidth, frameHeight);
    ASurfaceTransaction_setOnComplete(transaction, presentContext, onTransactionComplete);
    ASurfaceTransaction_apply(transaction);
    ASurfaceTransaction_delete(transaction);

    return true;
}

void SurfacePresenter::onShutdownComplete(void* context, ASurfaceTransactionStats* stats)
{
    auto* shutdown = static_cast<PresentContext*>(context);
    auto self = shutdown->self;
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        if (shutdown->previous) {
            int fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, shutdown->layer.get());
            self->m_freeBuffers.push_back({shutdown->previous, fence});
        }
        self->m_shutdownComplete = true;
        self->m_shutdownCv.notify_all();
    }
    delete shutdown;
}

// Tear down the surface control, waiting for SurfaceFlinger to commit the
// removal so no completion callbacks arrive after destruction.
void SurfacePresenter::destroySurfaceControlLocked(std::unique_lock<std::mutex>& lock)
{
    if (m_surfaceControl == nullptr) {
        return;
    }

    m_shutdownComplete = false;
    {
        ASurfaceTransaction* transaction = ASurfaceTransaction_create();
        ASurfaceTransaction_reparent(transaction, m_surfaceControl, nullptr);
        auto* shutdown = new PresentContext{shared_from_this(), m_presentedBuffer, m_layerOwner};
        m_presentedBuffer = nullptr;
        ASurfaceTransaction_setOnComplete(transaction, shutdown, onShutdownComplete);
        ASurfaceTransaction_apply(transaction);
        ASurfaceTransaction_delete(transaction);
    }

    if (!m_shutdownCv.wait_for(lock, std::chrono::milliseconds(500),
                               [this] { return m_shutdownComplete; })) {
        PLOGW("destroySurfaceControl: timeout waiting for teardown commit");
    }

    m_layerOwner.reset();
    m_surfaceControl = nullptr;
}

void SurfacePresenter::releaseBuffersLocked()
{
    for (const auto& entry : m_freeBuffers) {
        if (entry.fence >= 0) {
            close(entry.fence);
        }
        AHardwareBuffer_release(entry.buffer);
    }
    m_freeBuffers.clear();
    m_allocatedCount = 0;
    m_allocatedBytes = 0;
}
