// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Lab-only (dotfiles-vvt.10): EDEN_NX2_FENCE_TRACE=1 times every fence the fence manager queues
// through its submission: made (InnerFence::Queue) -> the submission holding it handed to the
// queue (vkQueueSubmit returned) -> the GPU starts and ends that submission (timestamps at its
// start and end) -> the fence manager runs its operations (a fence-delayed semaphore release
// lands in memory). GPU times map to the steady clock by the lower envelope of (GPU start -
// submit) over each 10 s window. Logged every 10 s with the GPU's busy share.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::FenceTrace {

inline const bool g_enabled = std::getenv("EDEN_NX2_FENCE_TRACE") != nullptr;

[[nodiscard]] inline s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Submission {
    s64 first_fence{}; ///< First fence made in it (CPU ns), 0 without fences
    u32 fences{};
    s64 submit{};      ///< vkQueueSubmit returned (CPU ns)
    s64 gpu_start{};   ///< GPU ns (its own clock)
    s64 gpu_end{};
    s64 released{};    ///< Its last fence's operations ran (CPU ns)
    bool gpu_known{};
};

struct State {
    std::mutex mutex;
    std::map<u64, Submission> ticks;
    s64 window_start{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void FenceMade(u64 tick) {
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Submission& sub = s.ticks[tick];
    if (sub.fences++ == 0) {
        sub.first_fence = now;
    }
}

inline void Submitted(u64 tick) {
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.ticks[tick].submit = now;
}

inline void GpuTimes(u64 tick, s64 start, s64 end) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Submission& sub = s.ticks[tick];
    sub.gpu_start = start;
    sub.gpu_end = end;
    sub.gpu_known = true;
}

inline void Report(State& s, s64 now);

inline void FenceReleased(u64 tick) {
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.ticks[tick].released = now;
    Report(s, now);
}

/// Every 10 s, over the submissions whose timestamps and fences are all in.
inline void Report(State& s, s64 now) {
    if (s.window_start == 0) {
        s.window_start = now;
    }
    if (now - s.window_start < 10'000'000'000LL) {
        return;
    }
    const double window_ms = static_cast<double>(now - s.window_start) / 1e6;
    s.window_start = now;
    // GPU clock -> steady clock: the largest (submit - GPU start) is the submission the GPU
    // started at once.
    s64 offset = std::numeric_limits<s64>::min();
    u64 complete{};
    for (const auto& [tick, sub] : s.ticks) {
        if (sub.gpu_known && sub.submit != 0) {
            offset = (std::max)(offset, sub.submit - sub.gpu_start);
            ++complete;
        }
    }
    if (complete == 0) {
        return;
    }
    struct Sum {
        double made_to_submit{}, submit_to_start{}, own{}, end_to_release{}, made_to_release{};
        u64 count{};
    } fenced;
    std::vector<std::pair<s64, s64>> busy;
    double own_all{};
    u64 submissions{};
    for (auto it = s.ticks.begin(); it != s.ticks.end();) {
        const Submission& sub = it->second;
        if (!sub.gpu_known || sub.submit == 0) {
            ++it;
            continue;
        }
        const s64 start = sub.gpu_start + offset;
        const s64 end = sub.gpu_end + offset;
        busy.emplace_back(start, end);
        own_all += static_cast<double>(end - start) / 1e6;
        ++submissions;
        if (sub.fences != 0 && sub.released != 0) {
            fenced.made_to_submit += static_cast<double>(sub.submit - sub.first_fence) / 1e6;
            fenced.submit_to_start += static_cast<double>(start - sub.submit) / 1e6;
            fenced.own += static_cast<double>(end - start) / 1e6;
            fenced.end_to_release += static_cast<double>(sub.released - end) / 1e6;
            fenced.made_to_release += static_cast<double>(sub.released - sub.first_fence) / 1e6;
            ++fenced.count;
        } else if (sub.fences != 0) {
            ++it; // its fences are not released yet
            continue;
        }
        it = s.ticks.erase(it);
    }
    std::ranges::sort(busy);
    s64 busy_ns{}, from{}, to{};
    for (const auto& [start, end] : busy) {
        if (start > to) {
            busy_ns += to - from;
            from = start;
            to = end;
        } else {
            to = (std::max)(to, end);
        }
    }
    busy_ns += to - from;
    const auto mean = [&](double sum) {
        return fenced.count != 0 ? sum / static_cast<double>(fenced.count) : 0.0;
    };
    LOG_INFO(Render_Vulkan,
             "Fence trace over {:.0f} ms: {} submissions ({:.3f} ms GPU each on average, GPU "
             "busy {:.1f}%), {} with fences; per fenced submission, mean ms: first fence made -> "
             "submitted {:.3f}, submitted -> GPU start {:.3f}, GPU run {:.3f}, GPU end -> fence "
             "operations ran {:.3f}, made -> ran {:.3f}",
             window_ms, submissions, submissions ? own_all / static_cast<double>(submissions) : 0.0,
             100.0 * static_cast<double>(busy_ns) / 1e6 / window_ms, fenced.count,
             mean(fenced.made_to_submit), mean(fenced.submit_to_start), mean(fenced.own),
             mean(fenced.end_to_release), mean(fenced.made_to_release));
}

} // namespace Vulkan::FenceTrace
