// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/kledd_cpu_monitor.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/hardware_properties.h"

namespace Core::Kledd {
namespace {

constexpr std::size_t MaxCores = Core::Hardware::NUM_CPU_CORES;
constexpr std::size_t MaxLogicalCpus = 256;

struct CoreSamples {
    std::atomic<unsigned> performance{};
    std::atomic<unsigned> efficiency{};
    std::atomic<unsigned> switches{}; ///< samples on a different host CPU than the previous one
    std::atomic<int> last_cpu{-1};
};

std::array<CoreSamples, MaxCores> core_samples{};
/// Efficiency class of each logical CPU (group 0), -1 when unknown
std::array<int, MaxLogicalCpus> cpu_class{};
int max_class = 0;
bool hybrid = false;
std::jthread monitor_thread;

void ReadTopology() {
    cpu_class.fill(-1);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        return;
    }
    std::vector<unsigned char> buffer(length);
    auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
    if (!GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0)) {
        return;
    }
    int min_class = 255;
    for (ULONG offset = 0; offset < length;) {
        auto* entry = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
        if (entry->Type == CpuSetInformation && entry->CpuSet.Group == 0) {
            const int cls = entry->CpuSet.EfficiencyClass;
            cpu_class[entry->CpuSet.LogicalProcessorIndex] = cls;
            max_class = (std::max)(max_class, cls);
            min_class = (std::min)(min_class, cls);
        }
        offset += entry->Size;
    }
    hybrid = max_class > min_class;
    int p = 0, e = 0;
    for (const int cls : cpu_class) {
        if (cls < 0) {
            continue;
        }
        (cls == max_class ? p : e)++;
    }
    LOG_INFO(Core, "[Kledd] CPU monitor: {} performance and {} efficiency logical CPUs{}", p, e,
             hybrid ? "" : " (not a hybrid CPU)");
}

std::string ThreadName(HANDLE handle, DWORD tid) {
    PWSTR wide = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(handle, &wide)) && wide) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1) {
            name.resize(static_cast<std::size_t>(bytes - 1));
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), bytes, nullptr, nullptr);
        }
        LocalFree(wide);
    }
    if (name.empty()) {
        name = "tid " + std::to_string(tid);
    }
    return name;
}

struct ThreadUsage {
    std::string name;
    double percent;
};

void MonitorLoop(std::stop_token token) {
    Common::SetCurrentThreadName("KleddCpuMonitor");
    const DWORD pid = GetCurrentProcessId();
    std::unordered_map<DWORD, unsigned long long> last_time; // 100 ns units, user + kernel
    auto last_wall = std::chrono::steady_clock::now();
    while (!token.stop_requested()) {
        for (int i = 0; i < 20 && !token.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (token.stop_requested()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const double wall_100ns =
            std::chrono::duration<double, std::ratio<1, 10'000'000>>(now - last_wall).count();
        last_wall = now;

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            continue;
        }
        std::vector<ThreadUsage> cores;
        std::vector<ThreadUsage> others;
        double total = 0.0;
        std::unordered_map<DWORD, unsigned long long> current;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            FILETIME created, exited, kernel_time, user_time;
            if (GetThreadTimes(thread, &created, &exited, &kernel_time, &user_time)) {
                const auto to_u64 = [](FILETIME t) {
                    return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) |
                           t.dwLowDateTime;
                };
                const unsigned long long used = to_u64(kernel_time) + to_u64(user_time);
                current[entry.th32ThreadID] = used;
                const auto it = last_time.find(entry.th32ThreadID);
                if (it != last_time.end() && wall_100ns > 0.0) {
                    const double percent =
                        100.0 * static_cast<double>(used - it->second) / wall_100ns;
                    total += percent;
                    std::string name = ThreadName(thread, entry.th32ThreadID);
                    if (name.rfind("CPUCore_", 0) == 0 || name == "CPUThread") {
                        cores.push_back({std::move(name), percent});
                    } else if (percent >= 3.0) {
                        others.push_back({std::move(name), percent});
                    }
                }
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        last_time = std::move(current);
        if (cores.empty()) {
            continue;
        }
        std::sort(cores.begin(), cores.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        std::sort(others.begin(), others.end(),
                  [](const auto& a, const auto& b) { return a.percent > b.percent; });
        if (others.size() > 8) {
            others.resize(8);
        }

        std::string core_text;
        for (const auto& c : cores) {
            const std::size_t index =
                c.name == "CPUThread" ? 0 : static_cast<std::size_t>(c.name.back() - '0');
            std::string where;
            if (index < MaxCores) {
                auto& s = core_samples[index];
                const unsigned p = s.performance.exchange(0, std::memory_order_relaxed);
                const unsigned e = s.efficiency.exchange(0, std::memory_order_relaxed);
                const unsigned moves = s.switches.exchange(0, std::memory_order_relaxed);
                if (hybrid && p + e > 0) {
                    where = fmt::format(", {}% on E-cores, {} moves", (e * 100) / (p + e), moves);
                } else if (p + e > 0) {
                    where = fmt::format(", {} moves", moves);
                }
            }
            core_text += fmt::format("{}{} {:.0f}%{}", core_text.empty() ? "" : "; ", c.name,
                                     c.percent, where);
        }
        std::string other_text;
        for (const auto& o : others) {
            other_text += fmt::format("{}{} {:.0f}%", other_text.empty() ? "" : ", ", o.name,
                                      o.percent);
        }
        LOG_INFO(Core,
                 "[Kledd] CPU use in the last 2 s (100% = one host core): {} | others: {} | "
                 "whole process {:.0f}%",
                 core_text, other_text.empty() ? "none above 3%" : other_text, total);
    }
}

} // namespace

void StartCpuMonitor() {
    if (!monitor_thread.joinable()) {
        ReadTopology();
        monitor_thread = std::jthread(MonitorLoop);
    }
}

void StopCpuMonitor() {
    if (monitor_thread.joinable()) {
        monitor_thread.request_stop();
        monitor_thread.join();
    }
}

void SampleCoreThread(std::size_t core) {
    if (core >= MaxCores) {
        return;
    }
    PROCESSOR_NUMBER number;
    GetCurrentProcessorNumberEx(&number);
    auto& s = core_samples[core];
    const int cpu = number.Group * 64 + number.Number;
    if (s.last_cpu.exchange(cpu, std::memory_order_relaxed) != cpu) {
        s.switches.fetch_add(1, std::memory_order_relaxed);
    }
    const int cls = (number.Group == 0) ? cpu_class[number.Number]
                                                                          : -1;
    if (cls < 0 || cls == max_class) {
        s.performance.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.efficiency.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Core::Kledd

#else

namespace Core::Kledd {
void StartCpuMonitor() {}
void StopCpuMonitor() {}
void SampleCoreThread(std::size_t) {}
} // namespace Core::Kledd

#endif
