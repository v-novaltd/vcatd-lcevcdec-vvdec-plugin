/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
*/

// Fixed-size pool for independent frame slices.

#ifndef VCAT_SLICE_THREAD_POOL_H
#define VCAT_SLICE_THREAD_POOL_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class SliceThreadPool
{
public:
    // The caller runs slice 0; values below 1 become a single slice.
    explicit SliceThreadPool(uint32_t threadCount);
    ~SliceThreadPool();

    SliceThreadPool(const SliceThreadPool&) = delete;
    SliceThreadPool& operator=(const SliceThreadPool&) = delete;

    uint32_t threadCount() const
    {
        return m_threadCount;
    }

    // Runs slice 0 on the caller and waits for all worker slices.
    // `fn` must be safe to invoke concurrently for different indices.
    void run(const std::function<void(uint32_t)>& fn);

private:
    void workerLoop(uint32_t sliceIndex);

    uint32_t m_threadCount;
    std::vector<std::thread> m_workers;

    std::mutex m_mutex;
    std::condition_variable m_workReady;
    std::condition_variable m_workDone;
    const std::function<void(uint32_t)>* m_fn = nullptr;
    uint64_t m_generation = 0;
    uint32_t m_pendingWorkers = 0;
    bool m_stop = false;
};

#endif // VCAT_SLICE_THREAD_POOL_H
