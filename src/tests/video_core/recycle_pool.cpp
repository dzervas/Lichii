// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/recycle_pool.h"

namespace {

using Pool = VideoCommon::RecyclePool<int, int>;

constexpr u64 MiB = u64{1} << 20;

} // namespace

TEST_CASE("A deleted resource is taken by the next one of its key, once",
          "[video_core][texture_cache][recycle_pool]") {
    Pool pool{256 * MiB, 8};
    pool.Put(1, 100, 24 * MiB);
    pool.Put(2, 200, 8 * MiB);
    pool.Put(1, 101, 24 * MiB);
    REQUIRE(!pool.Take(3));
    REQUIRE(pool.Take(1) == 100); // the oldest of its key
    REQUIRE(pool.Take(1) == 101);
    REQUIRE(!pool.Take(1));
    REQUIRE(pool.KeptBytes() == 8 * MiB);
    const Pool::Stats stats = pool.TakeStats();
    REQUIRE(stats.requests == 4);
    REQUIRE(stats.hits == 2);
    REQUIRE(stats.hit_bytes == 48 * MiB);
    REQUIRE(stats.kept_bytes == 8 * MiB);
    REQUIRE(pool.TakeStats().requests == 0);
}

TEST_CASE("Resources are kept a bounded time and size, the oldest dropped first",
          "[video_core][texture_cache][recycle_pool]") {
    Pool pool{64 * MiB, 2};
    pool.Put(1, 100, 24 * MiB);
    pool.Put(2, 200, 24 * MiB);
    pool.Put(3, 300, 24 * MiB); // over 64 MiB: drops 1
    REQUIRE(pool.KeptBytes() == 48 * MiB);
    REQUIRE(!pool.Take(1));
    pool.Put(4, 400, 65 * MiB); // larger than the bound alone: not kept
    REQUIRE(pool.KeptBytes() == 48 * MiB);
    pool.Tick();
    pool.Put(5, 500, 8 * MiB);
    pool.Tick();
    REQUIRE(pool.KeptBytes() == 56 * MiB); // 2 and 3 are 2 frames old
    pool.Tick();
    REQUIRE(pool.KeptBytes() == 8 * MiB);
    REQUIRE(pool.Take(5) == 500);
    REQUIRE(pool.KeptBytes() == 0);
}
