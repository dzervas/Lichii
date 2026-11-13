// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCommon {

/// Whether two images have the same shape: everything ImageInfo describes.
[[nodiscard]] inline bool SameImageInfo(const ImageInfo& lhs, const ImageInfo& rhs) {
    return lhs.format == rhs.format && lhs.type == rhs.type &&
           lhs.resources.levels == rhs.resources.levels &&
           lhs.resources.layers == rhs.resources.layers && lhs.size.width == rhs.size.width &&
           lhs.size.height == rhs.size.height && lhs.size.depth == rhs.size.depth &&
           lhs.block.width == rhs.block.width && lhs.block.height == rhs.block.height &&
           lhs.block.depth == rhs.block.depth && lhs.layer_stride == rhs.layer_stride &&
           lhs.maybe_unaligned_layer_stride == rhs.maybe_unaligned_layer_stride &&
           lhs.num_samples == rhs.num_samples && lhs.tile_width_spacing == rhs.tile_width_spacing &&
           lhs.native_tiling == rhs.native_tiling && lhs.rescaleable == rhs.rescaleable &&
           lhs.downscaleable == rhs.downscaleable && lhs.forced_flushed == rhs.forced_flushed &&
           lhs.dma_downloaded == rhs.dma_downloaded && lhs.is_sparse == rhs.is_sparse &&
           lhs.astc_opaque == rhs.astc_opaque;
}

/// Whether `older`, a bad overlap of a newer GPU-written image (they overlap, and neither fits in
/// the other, so nothing keeps their contents coherent), is deleted now rather than by the
/// collector later. `replaced`: it has the newer image's shape (JoinImages, on making the newer
/// one), or newer GPU-written images cover every byte of its guest range (TickFrame, see
/// GuestRangeCovered). Mario Kart World renders its 2048x2048 3-layer shadow map at 39-46
/// addresses within ~9.5 MiB, 96-144 KiB apart, and comes back to each one ~50-90 frames later:
/// about 30 of these 24 MiB arrays stayed resident on top of each other (0.7-1 GiB for ~34 MiB of
/// guest memory), only for the collector to delete them in its heavy mode and make them again
/// within 120 frames. Its dynamic-resolution targets of other sizes over one transient pool held
/// 3.8-4.3 GiB of bad overlaps for 1.3-1.4 GiB of guest memory in the heavy stretch: those go
/// once covered whole, as a part no newer image covers may still be read through. Only an image
/// written by the GPU and nothing since, with
/// many overlaps (BadOverlap: the collector deletes those without a download), no aliases and not
/// used this frame goes; DeleteImage keeps it until the GPU is done with it.
[[nodiscard]] constexpr bool SupersededByNewerImage(ImageFlagBits newer, ImageFlagBits older,
                                                    bool replaced, u64 older_last_use,
                                                    u64 frame_tick) {
    constexpr ImageFlagBits kept = ImageFlagBits::CpuModified | ImageFlagBits::Alias |
                                   ImageFlagBits::Sparse | ImageFlagBits::IsDecoding |
                                   ImageFlagBits::CostlyLoad;
    return replaced && True(newer & ImageFlagBits::GpuModified) &&
           True(older & ImageFlagBits::GpuModified) && True(older & ImageFlagBits::BadOverlap) &&
           True(older & ImageFlagBits::Registered) && False(older & kept) &&
           older_last_use < frame_tick;
}

/// Whether the ranges [first, second) cover every byte of [begin, end). Sorts `ranges`.
[[nodiscard]] inline bool GuestRangeCovered(u64 begin, u64 end,
                                            std::vector<std::pair<u64, u64>>& ranges) {
    std::ranges::sort(ranges);
    u64 covered = begin;
    for (const auto& [range_begin, range_end] : ranges) {
        if (range_begin > covered) {
            break;
        }
        covered = (std::max)(covered, range_end);
        if (covered >= end) {
            return true;
        }
    }
    return covered >= end;
}

} // namespace VideoCommon
