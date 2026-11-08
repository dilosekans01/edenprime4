// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace VideoCommon::Kledd {

/// eden-kledd diagnostic: blocking waits of the GPU thread (for the host GPU or the Vulkan worker),
/// grouped by the code that waited, summarised every 2 s by the GPU thread.
class WaitStats {
public:
    using Clock = std::chrono::steady_clock;

    static WaitStats& Get() {
        static WaitStats instance;
        return instance;
    }

    /// Marks the calling thread as the GPU thread; waits on other threads are counted apart
    static void MarkGpuThread() {
        OnGpuThread() = true;
    }

    void Add(const char* what, const std::source_location& loc, Clock::time_point start) {
        const auto us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        if (us < 100) {
            return;
        }
        std::string key = fmt::format("{}{} at {}:{} ({})", OnGpuThread() ? "" : "[other thread] ",
                                      what, ShortFile(loc.file_name()), loc.line(),
                                      loc.function_name());
        std::scoped_lock lock{mutex};
        auto& entry = entries[std::move(key)];
        entry.us += us;
        entry.count++;
        if (OnGpuThread()) {
            gpu_thread_wait_us += us;
        }
    }

    /// Returns the total waited on the GPU thread and the top callers, and resets
    std::string TakeSummary(uint64_t& gpu_thread_wait_ms) {
        std::vector<std::pair<std::string, Entry>> list;
        {
            std::scoped_lock lock{mutex};
            list.assign(entries.begin(), entries.end());
            entries.clear();
            gpu_thread_wait_ms = gpu_thread_wait_us / 1000;
            gpu_thread_wait_us = 0;
        }
        std::sort(list.begin(), list.end(),
                  [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
        std::string text;
        for (std::size_t i = 0; i < list.size() && i < 6; ++i) {
            text += fmt::format("{}{} ms in {} waits: {}", text.empty() ? "" : "; ",
                                list[i].second.us / 1000, list[i].second.count, list[i].first);
        }
        return text.empty() ? std::string{"none over 0.1 ms"} : text;
    }

private:
    struct Entry {
        uint64_t us = 0;
        uint64_t count = 0;
    };

    static bool& OnGpuThread() {
        thread_local bool value = false;
        return value;
    }

    static const char* ShortFile(const char* path) {
        const char* last = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '/' || *p == '\\') {
                last = p + 1;
            }
        }
        return last;
    }

    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    uint64_t gpu_thread_wait_us = 0;
};

} // namespace VideoCommon::Kledd
