// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Common::PageProtectionStats {

/// EDEN_NX2_GPU_PROFILE (Nx2Profile::ReadSwitch sets it): GPU-cached guest pages switched
/// between write-protected (cached) and writable (uncached), by direction and by whether the GPU
/// back thread or another thread (CPU cores' write traps, services) switched them.
inline bool g_enabled{};

struct Totals {
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> pages{};
    std::atomic<std::uint64_t> protect_ns{}; ///< Spent in the host mprotect
};

/// [cached][on the GPU back thread]
inline std::array<std::array<Totals, 2>, 2> g_totals{};
/// Guest CPU writes that reached the GPU caches through a write trap (HandleRasterizerWrite).
inline std::atomic<std::uint64_t> g_write_traps{};
inline thread_local bool t_gpu_back_thread{};

/// Pages switched since the last report, with the steady_clock time of their last unprotect
/// (0: not unprotected since it was last protected): how often each one flips, and how soon a
/// page a CPU write unprotected is protected again.
inline std::mutex g_pages_mutex;
inline std::unordered_map<std::uint64_t, std::uint64_t> g_pages;
/// Protected pages by the time since their unprotect: under 20 ms, 100 ms, 1 s, later.
inline std::array<std::uint64_t, 4> g_reprotect_ages{};

inline void Add(bool cached, std::uint64_t first_page, std::uint64_t num_pages,
                std::uint64_t protect_ns) {
    Totals& totals = g_totals[cached ? 1 : 0][t_gpu_back_thread ? 1 : 0];
    totals.calls.fetch_add(1, std::memory_order_relaxed);
    totals.pages.fetch_add(num_pages, std::memory_order_relaxed);
    totals.protect_ns.fetch_add(protect_ns, std::memory_order_relaxed);
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::scoped_lock lock{g_pages_mutex};
    for (std::uint64_t page = first_page; page != first_page + num_pages; ++page) {
        std::uint64_t& unprotected_at = g_pages[page];
        if (!cached) {
            unprotected_at = now;
            continue;
        }
        if (unprotected_at != 0) {
            const std::uint64_t age = now - unprotected_at;
            ++g_reprotect_ages[age < 20'000'000 ? 0 : age < 100'000'000 ? 1 : age < 1'000'000'000 ? 2 : 3];
            unprotected_at = 0;
        }
    }
}

} // namespace Common::PageProtectionStats
