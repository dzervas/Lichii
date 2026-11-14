// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <optional>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon {

/// Deleted resources kept a few frames and a bounded size, for a new one of the same key to take
/// instead of being made: oldest first, the oldest dropped first.
template <class Key, class Resource>
class RecyclePool {
public:
    struct Stats {
        u64 hits{};
        u64 requests{};
        u64 hit_bytes{};
        u64 kept_bytes{};
    };

    RecyclePool(u64 max_bytes_, u32 max_frames_) : max_bytes{max_bytes_}, max_frames{max_frames_} {}

    /// A kept resource of `key` (the oldest), or nothing.
    std::optional<Resource> Take(const Key& key) {
        ++stats.requests;
        const auto it =
            std::ranges::find_if(kept, [&key](const Kept& entry) { return entry.key == key; });
        if (it == kept.end()) {
            return std::nullopt;
        }
        Resource resource = std::move(it->resource);
        kept_bytes -= it->bytes;
        ++stats.hits;
        stats.hit_bytes += it->bytes;
        kept.erase(it);
        return resource;
    }

    /// Keeps `resource` of `bytes`, dropping the oldest ones past the size bound (or it, if it is
    /// larger than that alone).
    void Put(Key&& key, Resource&& resource, u64 bytes) {
        if (bytes > max_bytes) {
            return;
        }
        while (!kept.empty() && kept_bytes + bytes > max_bytes) {
            kept_bytes -= kept.front().bytes;
            kept.pop_front();
        }
        kept_bytes += bytes;
        kept.push_back({std::move(key), std::move(resource), bytes, 0});
    }

    /// A frame passed: drops what was kept longer than the frame bound.
    void Tick() {
        for (Kept& entry : kept) {
            ++entry.frames;
        }
        while (!kept.empty() && kept.front().frames > max_frames) {
            kept_bytes -= kept.front().bytes;
            kept.pop_front();
        }
    }

    /// The counts since the last call, and the bytes kept now.
    Stats TakeStats() {
        Stats taken = std::exchange(stats, {});
        taken.kept_bytes = kept_bytes;
        return taken;
    }

    [[nodiscard]] u64 KeptBytes() const noexcept {
        return kept_bytes;
    }

private:
    struct Kept {
        Key key;
        Resource resource;
        u64 bytes;
        u32 frames;
    };
    std::deque<Kept> kept;
    u64 kept_bytes{};
    u64 max_bytes;
    u32 max_frames;
    Stats stats;
};

} // namespace VideoCommon
