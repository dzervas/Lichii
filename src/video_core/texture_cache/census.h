// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Lab-only footprint census (dotfiles-vvt.2): EDEN_NX2_CENSUS=1 together with EDEN_NX2_TC_LOG=1
// logs, every 600 frames, where the texture cache's device memory goes: resident bytes by the age
// of their last actual use, per image class (format, type, size, levels, layers, samples) the
// bytes created, deleted (by reason) and held over their lifetime, who created the images and
// what used them in a later frame. Bytes are the VMA allocation sizes (Image::MemoryBlock), not
// the guest size estimate.

#include <array>
#include <cstdlib>
#include <map>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon::Census {

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_CENSUS") != nullptr;
    return enabled;
}

/// What looked an image up or used it: set around the lookup and use paths (Scope).
enum class Kind : u8 {
    None,         ///< No use in a later frame (first_later_use only)
    Draw,         ///< Sampled/storage views of draws and dispatches, and every other lookup
    RenderTarget, ///< Color and depth targets (UpdateRenderTargets, FindRenderTargetView)
    Blit,         ///< Fermi 2D blits and volume slice copies
    Dma,          ///< Maxwell DMA image <-> buffer copies
    AliasCopy,    ///< Alias synchronization copies (CopyImage), which never touch the LRU
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(Kind::Count)> kind_names{
    "none", "draw", "rt", "blit", "dma", "alias"};

/// Why an image was deleted.
enum class Delete : u8 {
    Collector,    ///< RunGarbageCollector
    Join,         ///< Replaced by a new image that JoinImages copied it into
    JoinRemapped, ///< Remapped overlap JoinImages discarded
    Unmap,        ///< Guest memory unmapped
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(Delete::Count)> delete_names{
    "gc", "join", "remap", "unmap"};

/// format, type, width, height, depth, levels, layers, samples
using ClassKey = std::array<u32, 8>;

struct Class {
    u64 created{};
    u64 created_bytes{};
    std::array<u64, static_cast<size_t>(Delete::Count)> deleted{};
    u64 deleted_bytes{};
    double deleted_byte_frames{}; ///< allocation bytes x frames alive, over the deleted images
    u64 deleted_unused{};         ///< deleted without an actual use after their creation frame
    std::array<u64, static_cast<size_t>(Kind::Count)> creators{};
    std::array<u64, static_cast<size_t>(Kind::Count)> first_later_use{}; ///< of deleted images
    u64 live_bytes{}; ///< kept across windows
    u64 live_count{};
    u64 live_peak{};  ///< highest live_bytes this window
};

/// Last-use age buckets (frames): <=1, <=10, <=120, <=600, <=1800, older.
inline constexpr std::array<u64, 5> age_limits{1, 10, 120, 600, 1800};
[[nodiscard]] inline size_t AgeBucket(u64 age) {
    size_t bucket = 0;
    while (bucket < age_limits.size() && age > age_limits[bucket]) {
        ++bucket;
    }
    return bucket;
}

struct Window {
    std::map<ClassKey, Class> classes;
    /// Deleted bytes and counts by reason and by the age of their last actual use.
    std::array<std::array<u64, age_limits.size() + 1>, static_cast<size_t>(Delete::Count)>
        deleted_bytes_by_age{};
    std::array<std::array<u64, age_limits.size() + 1>, static_cast<size_t>(Delete::Count)>
        deleted_count_by_age{};
    /// Deleted bytes by creator x first later use.
    std::array<std::array<u64, static_cast<size_t>(Kind::Count)>, static_cast<size_t>(Kind::Count)>
        deleted_bytes_by_kind{};
    u64 insert_ns{};    ///< InsertImage (lookup miss to registered image, joins and uploads)
    u64 insert_max_ns{};
    u64 inserts{};
    u32 identities_logged{}; ///< D16 2048x2048 deletions logged with their allocation
    u64 astc_alpha_sampled{}; ///< "alpha" ASTC images decoded on the CPU (at most 64)
    u64 astc_alpha_255{};     ///< of them, alpha 255 in every texel
    u64 astc_alpha_255_bytes{};
};

[[nodiscard]] inline bool IsShadowClass(const ClassKey& key, u32 d16_format) {
    return key[0] == d16_format && key[2] == 2048 && key[3] == 2048;
}

} // namespace VideoCommon::Census
