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

// SurfacePresenter - presents AHardwareBuffers to an ANativeWindow using
// SurfaceControl transactions (API 29+). Decode output pictures are backed by
// pool buffers acquired here and presented without any pixel copy; pictures the
// decoder cannot write directly are copied into CPU-writable pool buffers.

#ifndef LCEVC_DEC_JNI_SURFACE_PRESENTER_H
#define LCEVC_DEC_JNI_SURFACE_PRESENTER_H

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/surface_control.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class SurfacePresenter : public std::enable_shared_from_this<SurfacePresenter>
{
public:
    SurfacePresenter() = default;
    ~SurfacePresenter();

    SurfacePresenter(const SurfacePresenter&) = delete;
    SurfacePresenter& operator=(const SurfacePresenter&) = delete;

    // Attach to (or detach from, with nullptr) the output window. Acquires a
    // reference on the window and creates a child SurfaceControl for the video.
    bool setWindow(ANativeWindow* window);

    // True once a window has been attached
    bool ready();

    // Get a matching buffer, allocating if needed. The pool may contain multiple
    // formats and usages. Returns nullptr when the pool's memory budget is in use
    // (`exhausted` set, retry once buffers are recycled) or allocation fails.
    // `evicted` receives a stale free buffer the pool released to make room, so
    // callers can drop anything they keep for it; it must not be dereferenced.
    AHardwareBuffer* acquireBuffer(uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
                                   bool* exhausted = nullptr, AHardwareBuffer** evicted = nullptr);

    // Usage for buffers the CPU writes: CPU-writable, with overlay scan-out where
    // the device still allows such buffers to be CPU-locked.
    uint64_t cpuBufferUsage();

    // 8-bit 4:2:0 format for buffers the CPU writes: the explicit NV21 HAL format
    // where overlay buffers' flexible YUV locks as NV21 (some MediaTek composers
    // mis-scan flexible YUV buffers), otherwise AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420.
    uint32_t cpuYuv420Format();

    // Revert cpuYuv420Format() to flexible YUV, for decoders that cannot import
    // the explicit NV21 format.
    void disableExplicitNv21();

    // Return an unpresented buffer to the pool
    void recycleBuffer(AHardwareBuffer* buffer);

    // Present a buffer. The buffer returns to the pool automatically once a
    // later transaction replaces it on screen.
    bool presentBuffer(AHardwareBuffer* buffer, uint32_t frameWidth, uint32_t frameHeight);

private:
    // Travels with each presented buffer in the transaction completion callback
    struct PresentContext
    {
        std::shared_ptr<SurfacePresenter> self;
        AHardwareBuffer* previous;
        std::shared_ptr<ASurfaceControl> layer;
    };

    static void onTransactionComplete(void* context, ASurfaceTransactionStats* stats);
    static void onShutdownComplete(void* context, ASurfaceTransactionStats* stats);

    AHardwareBuffer* allocateBufferLocked(uint32_t width, uint32_t height, uint32_t format, uint64_t usage);
    uint64_t chooseBufferUsageLocked();
    bool probeLockableLocked(uint32_t format, uint64_t usage, bool* crFirst);
    void applyGeometryLocked(ASurfaceTransaction* transaction, uint32_t frameWidth, uint32_t frameHeight);
    void destroySurfaceControlLocked(std::unique_lock<std::mutex>& lock);
    void releaseBuffersLocked();

    std::mutex m_mutex;
    std::condition_variable m_shutdownCv;
    bool m_shutdownComplete = false;

    ANativeWindow* m_window = nullptr;
    ASurfaceControl* m_surfaceControl = nullptr;
    std::shared_ptr<ASurfaceControl> m_layerOwner;

    // Buffer pool
    struct FreeBuffer
    {
        AHardwareBuffer* buffer;
        int fence = -1;
    };
    std::vector<FreeBuffer> m_freeBuffers;
    uint32_t m_allocatedCount = 0;
    uint64_t m_allocatedBytes = 0;

    // Usage flags for CPU-written buffers, chosen by a one-off device probe (0 = not yet probed)
    uint64_t m_cpuBufferUsage = 0;
    // 8-bit format for CPU-written buffers (0 = not yet probed)
    uint32_t m_cpuYuv420Format = 0;

    // Buffer currently on screen (returned to the pool by the next completion)
    AHardwareBuffer* m_presentedBuffer = nullptr;

    // Geometry state - reapplied when the frame or window size changes
    uint32_t m_lastFrameWidth = 0;
    uint32_t m_lastFrameHeight = 0;
    uint32_t m_lastWindowWidth = 0;
    uint32_t m_lastWindowHeight = 0;
};

#endif // LCEVC_DEC_JNI_SURFACE_PRESENTER_H
