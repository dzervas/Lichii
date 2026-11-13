// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/superseded_overlap.h"

namespace {

using VideoCommon::ImageFlagBits;
using VideoCommon::ImageInfo;
using VideoCommon::SameImageInfo;
using VideoCommon::SupersededByNewerImage;

// Mario Kart World's shadow map: D16 2048x2048, 3 layers.
ImageInfo ShadowArray() {
    ImageInfo info;
    info.format = VideoCore::Surface::PixelFormat::D16_UNORM;
    info.type = VideoCommon::ImageType::e2D;
    info.size = {2048, 2048, 1};
    info.resources.levels = 1;
    info.resources.layers = 3;
    info.block = {0, 4, 0};
    info.layer_stride = 8u << 20;
    info.maybe_unaligned_layer_stride = 8u << 20;
    return info;
}

constexpr ImageFlagBits Written = ImageFlagBits::GpuModified;
constexpr ImageFlagBits OlderWritten =
    ImageFlagBits::GpuModified | ImageFlagBits::BadOverlap | ImageFlagBits::Registered |
    ImageFlagBits::Tracked | ImageFlagBits::Strong;

} // namespace

TEST_CASE("A GPU-written image supersedes older bad overlaps of its shape",
          "[video_core][texture_cache][superseded_overlap]") {
    // The array MKW rendered 96 KiB away 60 frames ago.
    REQUIRE(SameImageInfo(ShadowArray(), ShadowArray()));
    REQUIRE(SupersededByNewerImage(Written, OlderWritten, true, 940, 1000));
    REQUIRE(SupersededByNewerImage(Written, OlderWritten, true, 999, 1000));
}

TEST_CASE("Images of another shape, used this frame or holding other data stay",
          "[video_core][texture_cache][superseded_overlap]") {
    ImageInfo single = ShadowArray();
    single.resources.layers = 1;
    REQUIRE_FALSE(SameImageInfo(ShadowArray(), single));
    ImageInfo color = ShadowArray();
    color.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    REQUIRE_FALSE(SameImageInfo(ShadowArray(), color));
    ImageInfo stride = ShadowArray();
    stride.layer_stride += 4096;
    REQUIRE_FALSE(SameImageInfo(ShadowArray(), stride));
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten, false, 940, 1000));

    // Used this frame (its views may be bound for the draw being set up).
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten, true, 1000, 1000));
    // The new image holds no GPU-written data yet (a render target before its first draw).
    REQUIRE_FALSE(SupersededByNewerImage(ImageFlagBits::CpuModified, OlderWritten, true, 940, 1000));
    // The older one holds guest data rather than the GPU's, or the GPU's plus later CPU writes.
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten & ~ImageFlagBits::GpuModified, true,
                                         940, 1000));
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten | ImageFlagBits::CpuModified, true,
                                         940, 1000));
    // A single overlap: not BadOverlap, so the collector would download it first.
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten & ~ImageFlagBits::BadOverlap, true,
                                         940, 1000));
    // Aliases are kept coherent by copies; sparse, decoding and costly images are left alone.
    for (const ImageFlagBits kept : {ImageFlagBits::Alias, ImageFlagBits::Sparse,
                                     ImageFlagBits::IsDecoding, ImageFlagBits::CostlyLoad}) {
        REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten | kept, true, 940, 1000));
    }
    REQUIRE_FALSE(SupersededByNewerImage(Written, OlderWritten & ~ImageFlagBits::Registered, true,
                                         940, 1000));
}

TEST_CASE("Newer images must cover every byte of an older one of another shape",
          "[video_core][texture_cache][superseded_overlap]") {
    using VideoCommon::GuestRangeCovered;
    // A 1872x1048 RGBA8 target (7.5 MiB) at 0x1000000, rendered over by targets of other sizes.
    constexpr u64 begin = 0x1000000;
    constexpr u64 end = begin + 1872 * 1048 * 4;
    std::vector<std::pair<u64, u64>> ranges{{begin, end + 4096}};
    REQUIRE(GuestRangeCovered(begin, end, ranges));
    // Together, in any order, overlapping or touching.
    ranges = {{begin + 0x400000, end}, {begin - 0x1000, begin + 0x200000},
              {begin + 0x200000, begin + 0x500000}};
    REQUIRE(GuestRangeCovered(begin, end, ranges));
    // A gap: those bytes may still be read through the older image.
    ranges = {{begin, begin + 0x200000}, {begin + 0x200001, end}};
    REQUIRE_FALSE(GuestRangeCovered(begin, end, ranges));
    // Short of either end.
    ranges = {{begin + 1, end}};
    REQUIRE_FALSE(GuestRangeCovered(begin, end, ranges));
    ranges = {{begin, end - 1}};
    REQUIRE_FALSE(GuestRangeCovered(begin, end, ranges));
    ranges.clear();
    REQUIRE_FALSE(GuestRangeCovered(begin, end, ranges));
    // Covered whole, the policy applies as for the same shape.
    REQUIRE(SupersededByNewerImage(Written, OlderWritten, true, 940, 1000));
}
