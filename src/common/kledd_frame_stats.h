// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

namespace Common::Kledd {

/// eden-kledd: per-frame counters for the long-frame log (stutter investigation).
/// Written from the GPU and pipeline worker threads, read and reset once per frame.
struct FrameStats {
    std::atomic<u32> images_created{};
    std::atomic<u32> images_deleted{};
    std::atomic<u32> gc_deleted{};
    std::atomic<u32> gc_sync_downloads{};
    std::atomic<u32> join_dropped_gpu{};
    std::atomic<u32> uploads{};
    std::atomic<u64> upload_bytes{};
    std::atomic<u32> async_decodes{};
    std::atomic<u32> async_unswizzles{};
    std::atomic<u32> graphics_pipelines{};
    std::atomic<u32> compute_pipelines{};
    /// Microseconds spent this frame in texture cache work (GPU thread)
    std::atomic<u64> insert_us{};       ///< InsertImage: finding overlaps, creating, first upload
    std::atomic<u64> create_us{};       ///< creating the host image (allocation, views)
    std::atomic<u64> refresh_us{};      ///< RefreshContents: every upload of CPU-modified images
    std::atomic<u64> plain_upload_us{}; ///< whole-image uploads (not the watched pools)
    std::atomic<u64> plain_upload_max_us{};
    std::atomic<u64> plain_upload_max_bytes{};
};

/// Adds the time since start to counter, in microseconds
template <typename Clock, typename TimePoint>
inline u64 AddElapsedUs(std::atomic<u64>& counter, TimePoint start) {
    const u64 us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    counter.fetch_add(us, std::memory_order_relaxed);
    return us;
}

inline FrameStats& GetFrameStats() {
    static FrameStats stats;
    return stats;
}

} // namespace Common::Kledd
