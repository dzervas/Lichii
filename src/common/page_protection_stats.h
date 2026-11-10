// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>

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

/// Distinct pages switched since the last report: with the totals, how often each one flips.
inline std::mutex g_pages_mutex;
inline std::unordered_set<std::uint64_t> g_pages;

inline void Add(bool cached, std::uint64_t first_page, std::uint64_t num_pages,
                std::uint64_t protect_ns) {
    Totals& totals = g_totals[cached ? 1 : 0][t_gpu_back_thread ? 1 : 0];
    totals.calls.fetch_add(1, std::memory_order_relaxed);
    totals.pages.fetch_add(num_pages, std::memory_order_relaxed);
    totals.protect_ns.fetch_add(protect_ns, std::memory_order_relaxed);
    std::scoped_lock lock{g_pages_mutex};
    for (std::uint64_t page = first_page; page != first_page + num_pages; ++page) {
        g_pages.insert(page);
    }
}

} // namespace Common::PageProtectionStats
