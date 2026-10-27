/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "slice_thread_pool.h"

SliceThreadPool::SliceThreadPool(uint32_t threadCount)
    : m_threadCount(threadCount < 1 ? 1 : threadCount)
{
    m_workers.reserve(m_threadCount - 1);
    for (uint32_t i = 1; i < m_threadCount; ++i) {
        m_workers.emplace_back([this, i] { workerLoop(i); });
    }
}

SliceThreadPool::~SliceThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workReady.notify_all();
    for (auto& worker : m_workers) {
        worker.join();
    }
}

void SliceThreadPool::workerLoop(uint32_t sliceIndex)
{
    uint64_t seenGeneration = 0;
    while (true) {
        const std::function<void(uint32_t)>* fn;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workReady.wait(lock, [&] { return m_stop || m_generation != seenGeneration; });
            if (m_stop) {
                return;
            }
            seenGeneration = m_generation;
            fn = m_fn;
        }
        (*fn)(sliceIndex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (--m_pendingWorkers == 0) {
                m_workDone.notify_one();
            }
        }
    }
}

void SliceThreadPool::run(const std::function<void(uint32_t)>& fn)
{
    if (m_threadCount <= 1) {
        fn(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fn = &fn;
        m_pendingWorkers = m_threadCount - 1;
        ++m_generation;
    }
    m_workReady.notify_all();
    fn(0); // This thread's own slice, while the workers run theirs.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_workDone.wait(lock, [&] { return m_pendingWorkers == 0; });
}
