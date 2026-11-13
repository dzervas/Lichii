// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

/// The least device memory the collector lets the texture cache fill before it collects in
/// earnest, whatever the card (4d8a9629a: Mario Kart World kept 6.7 GB of an 8 GB card).
constexpr u64 MinCriticalMemoryCap = u64{5} << 30;

/// Device budget that cards larger than 8 GB keep free under the collector's critical threshold:
/// what an 8 GB card's ~7 GB budget leaves under MinCriticalMemoryCap.
constexpr u64 CriticalMemoryHeadroom = u64{2} << 30;

/// The most device memory the collector lets the cache fill before it collects in earnest, for a
/// driver budget of `budget` (0 when the driver reports none). 5/8 of the budget (PR #268) put
/// it at 8.1 GiB on a 16 GB card (12.9 GiB budget), under Mario Kart World's Knockout Tour
/// working set: the collector deleted and created ~20k images every 600 frames, re-uploading
/// 4-6 GiB, and frames took 37-46 ms instead of ~25 ms.
constexpr u64 CriticalMemoryCap(u64 budget) {
    return (std::max)(MinCriticalMemoryCap, budget - (std::min)(budget, CriticalMemoryHeadroom));
}

/// Device budget left free, past the allocator's reserves, where the collector starts deleting
/// recently used images: room for the images made between two budget checks (30 frames).
constexpr u64 DeviceLimitHeadroom = u64{512} << 20;

/// The usage past which the collector deletes images however recently they were used, for a
/// driver budget of `budget` and `reserve`, the room kept for the allocator's block slack and
/// after images did not fit. The collector's own margin under the budget (1/8 of it, at most
/// 1 GiB) is left out: holding the working set there costs nothing. Past the budget the allocator
/// places images in host memory (VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT, then another memory
/// type), which is far worse than collecting: Mario Kart World with 1.7 GiB of images there drew
/// 50-100 ms frames, against 35-45 ms while the collector deleted and made images again.
constexpr u64 DeviceLimitMemory(u64 budget, u64 reserve) {
    return budget - (std::min)(reserve + DeviceLimitHeadroom, budget / 2);
}

/// Frames an image must have gone unused before the collector deletes it, short of
/// DeviceLimitMemory. A working set over the thresholds otherwise has its images deleted between
/// two uses and created and uploaded again each time (Mario Kart World's Knockout Tour: 33 images
/// a frame at the 10-frame age of aggressive collection).
constexpr u64 RecentUseFrames = 120;

/// The age the collector deletes images from: `mode_age`, the one its mode asks for, but no
/// younger than RecentUseFrames until the usage reaches DeviceLimitMemory (`at_device_limit`) or
/// an image could not be made at all (`out_of_memory`). Past those, images that are still used go
/// too: past the budget images land in host memory, and an image that cannot be made draws as
/// garbage.
constexpr u64 CollectableAge(u64 mode_age, bool at_device_limit, bool out_of_memory) {
    if (at_device_limit || out_of_memory) {
        return mode_age;
    }
    return (std::max)(mode_age, RecentUseFrames);
}

} // namespace VideoCommon
