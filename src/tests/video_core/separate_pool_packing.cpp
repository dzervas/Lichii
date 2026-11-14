// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_device.h"

namespace {

using Vulkan::Device;

constexpr VkDeviceSize KiB = 1ULL << 10;
constexpr VkDeviceSize MiB = 1ULL << 20;

} // namespace

TEST_CASE("Sparse pages the pool's blocks hold with little left over go to the pool",
          "[video_core][vulkan][separate_pool]") {
    REQUIRE(Device::PacksIntoSeparatePool(64 * KiB));    // a sparse block
    REQUIRE(Device::PacksIntoSeparatePool(1408 * KiB));  // 11 to a block, 0.875 MiB left
    REQUIRE(Device::PacksIntoSeparatePool(4 * MiB));
    REQUIRE(Device::PacksIntoSeparatePool(8 * MiB));
    REQUIRE(Device::PacksIntoSeparatePool(16 * MiB));
}

TEST_CASE("Sparse pages that leave much of a pool block empty get their own memory",
          "[video_core][vulkan][separate_pool]") {
    REQUIRE(!Device::PacksIntoSeparatePool(0));
    REQUIRE(!Device::PacksIntoSeparatePool(5504 * KiB)); // 2 to a block, 5.25 MiB left
    REQUIRE(!Device::PacksIntoSeparatePool(6 * MiB));
    REQUIRE(!Device::PacksIntoSeparatePool(9 * MiB));
    REQUIRE(!Device::PacksIntoSeparatePool(17 * MiB));   // larger than a block
}
