// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {

using VideoCommon::ImageInfo;
using VideoCommon::LevelRect;
using VideoCommon::SparseLayerTileRects;
using VideoCommon::SparseTileRects;

constexpr u64 KiB = 1024;

ImageInfo Array(VideoCore::Surface::PixelFormat format, u32 size, s32 levels) {
    ImageInfo info;
    info.format = format;
    info.type = VideoCommon::ImageType::e2D;
    info.size = {size, size, 1};
    info.resources.levels = levels;
    info.resources.layers = 68;
    info.block = {0, 4, 0}; // 16 GOBs tall
    return info;
}

bool Same(const std::vector<LevelRect>& lhs, const std::vector<LevelRect>& rhs) {
    return std::ranges::equal(lhs, rhs, [](const LevelRect& a, const LevelRect& b) {
        return a.level == b.level && a.x == b.x && a.y == b.y && a.width == b.width &&
               a.height == b.height;
    });
}

} // namespace

TEST_CASE("Sparse blocks of one layer of a 1x1-texel array are those of a single image",
          "[video_core][texture_cache][sparse_layer_tiles]") {
    ImageInfo single = Array(VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM, 1024, 1);
    const std::vector<std::pair<u64, u64>> ranges{{0, 64 * KiB}, {1024 * KiB, 4096 * KiB}};
    const std::vector<LevelRect> layer = SparseLayerTileRects(single, ranges, 128, 128, 1, 0);
    single.resources.layers = 1;
    REQUIRE(Same(layer, SparseTileRects(single, ranges, 128, 128, 1)));
    REQUIRE(!layer.empty());
}

TEST_CASE("Sparse blocks of a block-compressed layer are in texels",
          "[video_core][texture_cache][sparse_layer_tiles]") {
    // BC1, 1024x1024 texels = 256x256 blocks of 8 bytes: a row of blocks is 2048 bytes, 32 GOBs
    // across; a block-linear block is 16 GOBs (8 KiB), 8 format blocks (32 texels) across and 128
    // (512 texels) down. The first 64 KiB are 8 such blocks: texels [0, 256) x [0, 512).
    // Host sparse blocks of BC1 are 128x64 format blocks, 512x256 texels.
    const ImageInfo info = Array(VideoCore::Surface::PixelFormat::BC1_RGBA_UNORM, 1024, 1);
    const std::vector<std::pair<u64, u64>> ranges{{0, 64 * KiB}};
    const std::vector<LevelRect> rects = SparseLayerTileRects(info, ranges, 512, 256, 1, 0);
    REQUIRE(Same(rects, {{0, 0, 0, 512, 256}, {0, 0, 256, 512, 256}}));
}

TEST_CASE("Sparse blocks skip the guest levels the host leaves out",
          "[video_core][texture_cache][sparse_layer_tiles]") {
    // RGBA8 1024x1024 with 3 levels: level 0 is 4 MiB, level 1 starts there. The host drops level
    // 0 (TrimmedLevels): data only in level 0 binds nothing, data at level 1's start binds host
    // level 0's first block; host level 1 is the mip tail and comes whole.
    const ImageInfo info = Array(VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM, 1024, 3);
    const std::vector<std::pair<u64, u64>> level0{{0, 64 * KiB}};
    REQUIRE(SparseLayerTileRects(info, level0, 128, 128, 1, 1).empty());
    const std::vector<std::pair<u64, u64>> level1{{4096 * KiB, 4 * KiB}};
    const std::vector<LevelRect> rects = SparseLayerTileRects(info, level1, 128, 128, 1, 1);
    REQUIRE(rects.size() == 1);
    REQUIRE(rects[0].level == 0);
    REQUIRE(rects[0].x == 0);
    REQUIRE(rects[0].y == 0);
    const std::vector<std::pair<u64, u64>> level2{{5120 * KiB, 4 * KiB}};
    REQUIRE(Same(SparseLayerTileRects(info, level2, 128, 128, 1, 1), {{1, 0, 0, 256, 256}}));
}
