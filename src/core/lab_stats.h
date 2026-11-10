// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Lab only (dotfiles-m0h): with EDEN_LAB_STATS set, game frame intervals at the Switch 1
// compositor plus SVC, SleepThread and yield counts, logged every 300 game frames.

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

namespace Core::LabStats {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_LAB_STATS") != nullptr;
    return enabled;
}

// Indexed by SVC number.
inline std::array<std::atomic<u64>, 0x100> svc_counts{};

enum Sleep : std::size_t { SleepPositive, SleepYield0, SleepYieldMinus1, SleepYieldMinus2, SleepOther,
                           SleepKinds };
inline std::array<std::atomic<u64>, SleepKinds> sleep_counts{};

// Yields that returned at the "yield count matches" check, by yield kind (0, -1, -2).
inline std::array<std::atomic<u64>, 3> yield_early_outs{};
// Yields that changed the scheduled thread (SetSchedulerUpdateNeeded), by yield kind.
inline std::array<std::atomic<u64>, 3> yield_switches{};

inline void Count(std::atomic<u64>& counter) {
    if (Enabled()) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}

// Called once per new game (non-overlay) buffer at the compositor.
void OnGameFrame();

} // namespace Core::LabStats
