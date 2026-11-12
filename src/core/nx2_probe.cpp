// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "core/nx2_probe.h"

namespace Core::Nx2Probe {
namespace {

constexpr u32 ThreadSlots = 1024;

std::FILE* Out() {
    static std::FILE* const file = [] {
        const char* path = std::getenv("EDEN_NX2_PROBE_OUT");
        return path != nullptr ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

std::mutex out_mutex;

struct PcCounts {
    std::unordered_set<u64> probes;
    std::unordered_map<u64, u32> index;
    std::vector<u64> pcs;
    std::unique_ptr<std::atomic<u64>[]> counts;
    u64 every = 300;
};

const PcCounts* LoadPcCounts() {
    const char* path = std::getenv("EDEN_NX2_PC_COUNT");
    if (path == nullptr) {
        return nullptr;
    }
    auto* counts = new PcCounts;
    std::ifstream in{path};
    std::string word;
    while (in >> word) {
        const u64 pc = std::strtoull(word.c_str(), nullptr, 16);
        if (pc != 0 && counts->index.emplace(pc, static_cast<u32>(counts->pcs.size())).second) {
            counts->pcs.push_back(pc);
            counts->probes.insert(pc);
        }
    }
    counts->counts = std::make_unique<std::atomic<u64>[]>(counts->pcs.size() * ThreadSlots);
    if (const char* every = std::getenv("EDEN_NX2_PC_COUNT_EVERY")) {
        counts->every = std::max<u64>(std::strtoull(every, nullptr, 10), 1);
    }
    return counts;
}

const PcCounts* GetPcCounts() {
    static const PcCounts* const counts = LoadPcCounts();
    return counts;
}

struct Waits {
    std::mutex mutex;
    std::map<std::pair<u32, std::string>, std::pair<u64, u64>> by_kind; // count, ns
    std::unordered_map<u32, std::string> requests;
};

Waits& GetWaits() {
    static Waits waits;
    return waits;
}

} // namespace

const std::unordered_set<u64>* BlockEntryProbes() {
    const PcCounts* counts = GetPcCounts();
    return counts != nullptr ? &counts->probes : nullptr;
}

void CountBlockEntry(u64 pc, u32 thread_id) {
    const PcCounts* counts = GetPcCounts();
    if (counts == nullptr) {
        return;
    }
    const auto it = counts->index.find(pc);
    if (it != counts->index.end()) {
        counts->counts[it->second * ThreadSlots + (thread_id % ThreadSlots)].fetch_add(
            1, std::memory_order_relaxed);
    }
}

bool PaceTraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_PACE_TRACE") != nullptr;
    return enabled;
}

void NoteRequest(u32 client_thread_id, std::string_view service, u32 command) {
    auto& waits = GetWaits();
    std::scoped_lock lock{waits.mutex};
    waits.requests[client_thread_id] = fmt::format("{}:{}", service, command);
}

std::string TakeRequest(u32 thread_id) {
    auto& waits = GetWaits();
    std::scoped_lock lock{waits.mutex};
    const auto it = waits.requests.find(thread_id);
    if (it == waits.requests.end()) {
        return "?";
    }
    std::string kind = std::move(it->second);
    waits.requests.erase(it);
    return kind;
}

void NoteWait(u32 thread_id, const std::string& kind, u64 ns) {
    auto& waits = GetWaits();
    std::scoped_lock lock{waits.mutex};
    auto& entry = waits.by_kind[{thread_id, kind}];
    ++entry.first;
    entry.second += ns;
}

void OnPresent(const PresentTiming& timing) {
    std::FILE* out = Out();
    if (out == nullptr) {
        return;
    }
    std::string text;
    if (PaceTraceEnabled()) {
        text += fmt::format("p {} {} {} {} {} {} {}\n", timing.present, timing.thread_id,
                            timing.entered_ns, timing.fence_us, timing.composite_us,
                            timing.pace_us, timing.pace_requested_us);
        auto& waits = GetWaits();
        std::scoped_lock lock{waits.mutex};
        for (const auto& [key, value] : waits.by_kind) {
            text += fmt::format("w {} {} {} {} {}\n", timing.present, key.first, key.second,
                                value.first, value.second / 1000);
        }
        waits.by_kind.clear();
    }
    if (const PcCounts* counts = GetPcCounts(); counts != nullptr && timing.present % counts->every == 0) {
        for (size_t i = 0; i < counts->pcs.size(); ++i) {
            for (u32 thread = 0; thread < ThreadSlots; ++thread) {
                const u64 count =
                    counts->counts[i * ThreadSlots + thread].load(std::memory_order_relaxed);
                if (count != 0) {
                    text += fmt::format("c {} {:#x} {} {}\n", timing.present, counts->pcs[i],
                                        thread, count);
                }
            }
        }
    }
    std::scoped_lock lock{out_mutex};
    std::fwrite(text.data(), 1, text.size(), out);
    std::fflush(out);
}

} // namespace Core::Nx2Probe
