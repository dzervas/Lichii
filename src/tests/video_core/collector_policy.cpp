// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/collector_policy.h"

namespace {

constexpr u64 MiB = u64{1} << 20;
constexpr u64 GiB = u64{1} << 30;

} // namespace

TEST_CASE("Cards up to 8 GB keep the 5 GiB critical cap",
          "[video_core][texture_cache][collector_policy]") {
    REQUIRE(VideoCommon::CriticalMemoryCap(0) == VideoCommon::MinCriticalMemoryCap);
    REQUIRE(VideoCommon::CriticalMemoryCap(1 * GiB) == VideoCommon::MinCriticalMemoryCap);
    // An 8 GB card's budget.
    REQUIRE(VideoCommon::CriticalMemoryCap(7 * GiB) == 5 * GiB);
}

TEST_CASE("Larger cards fill their budget up to the headroom before collecting",
          "[video_core][texture_cache][collector_policy]") {
    // RTX 5080 16 GB: 12.9 GiB budget, Mario Kart World's Knockout Tour needs ~9-10 GiB.
    REQUIRE(VideoCommon::CriticalMemoryCap(13222 * MiB) == 11174 * MiB);
    REQUIRE(VideoCommon::CriticalMemoryCap(13222 * MiB) > 13222 * MiB / 8 * 5);
    // A 12 GB card's ~11.2 GB budget.
    REQUIRE(VideoCommon::CriticalMemoryCap(11 * GiB) == 9 * GiB);
}

TEST_CASE("Recently used images stay until the budget's reserves and headroom",
          "[video_core][texture_cache][collector_policy]") {
    using VideoCommon::DeviceLimitMemory;
    // 12.9 GiB budget with 1.5 GiB of block slack: critical sits at 10662 MiB (1 GiB margin).
    REQUIRE(DeviceLimitMemory(13222 * MiB, 1536 * MiB) == 11174 * MiB);
    REQUIRE(DeviceLimitMemory(13222 * MiB, 1536 * MiB) > 13222 * MiB - 1 * GiB - 1536 * MiB);
    REQUIRE(DeviceLimitMemory(8 * GiB, 0) == 8 * GiB - VideoCommon::DeviceLimitHeadroom);
    // Never under half the budget.
    REQUIRE(DeviceLimitMemory(4 * GiB, 3 * GiB) == 2 * GiB);
}

TEST_CASE("The collector keeps recently used images while the device has room",
          "[video_core][texture_cache][collector_policy]") {
    using VideoCommon::CollectableAge;
    using VideoCommon::RecentUseFrames;
    REQUIRE(CollectableAge(10, false, false) == RecentUseFrames);
    REQUIRE(CollectableAge(50, false, false) == RecentUseFrames);
    REQUIRE(CollectableAge(RecentUseFrames + 1, false, false) == RecentUseFrames + 1);
}

TEST_CASE("The collector deletes recently used images at the device limit or out of memory",
          "[video_core][texture_cache][collector_policy]") {
    using VideoCommon::CollectableAge;
    REQUIRE(CollectableAge(10, true, false) == 10);
    REQUIRE(CollectableAge(10, false, true) == 10);
    REQUIRE(CollectableAge(25, true, true) == 25);
}
