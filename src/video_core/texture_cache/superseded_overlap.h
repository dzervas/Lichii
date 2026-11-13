// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

/// Whether JoinImages deletes `older`, a bad overlap of the image it just made (they overlap, and
/// neither fits in the other, so nothing keeps their contents coherent), as soon as the new one
/// holds GPU-written data. Mario Kart World renders its 2048x2048 3-layer shadow map at 39-46
/// addresses within ~9.5 MiB, 96-144 KiB apart, and comes back to each one ~50-90 frames later:
/// about 30 of these 24 MiB arrays stayed resident on top of each other (0.7-1 GiB for ~34 MiB of
/// guest memory), only for the collector to delete them in its heavy mode and make them again
/// within 120 frames. Only an image of the same shape, written by the GPU and nothing since, with
/// many overlaps (BadOverlap: the collector deletes those without a download), no aliases and not
/// used this frame goes; DeleteImage keeps it until the GPU is done with it.
[[nodiscard]] constexpr bool SupersededByNewerImage(ImageFlagBits newer, ImageFlagBits older,
                                                    bool same_info, u64 older_last_use,
                                                    u64 frame_tick) {
    constexpr ImageFlagBits kept = ImageFlagBits::CpuModified | ImageFlagBits::Alias |
                                   ImageFlagBits::Sparse | ImageFlagBits::IsDecoding |
                                   ImageFlagBits::CostlyLoad;
    return same_info && True(newer & ImageFlagBits::GpuModified) &&
           True(older & ImageFlagBits::GpuModified) && True(older & ImageFlagBits::BadOverlap) &&
           True(older & ImageFlagBits::Registered) && False(older & kept) &&
           older_last_use < frame_tick;
}

} // namespace VideoCommon
