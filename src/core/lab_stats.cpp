// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/lab_stats.h"

namespace Core::LabStats {

namespace {

constexpr std::size_t Window = 300;

template <std::size_t N>
std::array<u64, N> Snapshot(const std::array<std::atomic<u64>, N>& counters) {
    std::array<u64, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = counters[i].load(std::memory_order_relaxed);
    }
    return out;
}

struct State {
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_frame{};
    std::chrono::steady_clock::time_point window_start{};
    std::vector<double> intervals_ms;
    std::array<u64, 0x100> svc{};
    std::array<u64, SleepKinds> sleep{};
    std::array<u64, 3> early{};
    std::array<u64, 3> switches{};
};

State& GetState() {
    static State state;
    return state;
}

} // namespace

void OnGameFrame() {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (s.last_frame == std::chrono::steady_clock::time_point{}) {
        s.last_frame = s.window_start = now;
        s.svc = Snapshot(svc_counts);
        s.sleep = Snapshot(sleep_counts);
        s.early = Snapshot(yield_early_outs);
        s.switches = Snapshot(yield_switches);
        return;
    }
    const double interval_ms = std::chrono::duration<double, std::milli>(now - s.last_frame).count();
    s.intervals_ms.push_back(interval_ms);
    if (interval_ms > 45.0) {
        // steady_clock is CLOCK_MONOTONIC on Linux: matches `perf record -k CLOCK_MONOTONIC`.
        LOG_INFO(Debug, "LabStats long frame: end_ns={} ms={:.1f}",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                 interval_ms);
    }
    s.last_frame = now;
    if (s.intervals_ms.size() < Window) {
        return;
    }

    const double seconds = std::chrono::duration<double>(now - s.window_start).count();
    auto sorted = s.intervals_ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double v : sorted) {
        sum += v;
    }
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1,
                                                            static_cast<std::size_t>(p * sorted.size()))]; };
    const auto over = [&](double ms) {
        return std::count_if(sorted.begin(), sorted.end(), [ms](double v) { return v > ms; });
    };
    LOG_INFO(Debug, "LabStats frames: n={} fps={:.1f} mean={:.1f} p50={:.1f} p95={:.1f} p99={:.1f} "
                    "max={:.1f} ms, >40ms={} >50ms={} >70ms={}",
             sorted.size(), sorted.size() / seconds, sum / sorted.size(), pct(0.5), pct(0.95),
             pct(0.99), sorted.back(), over(40), over(50), over(70));

    const auto svc = Snapshot(svc_counts);
    std::vector<std::pair<u64, std::size_t>> rates;
    u64 total = 0;
    for (std::size_t i = 0; i < svc.size(); ++i) {
        const u64 delta = svc[i] - s.svc[i];
        total += delta;
        if (delta != 0) {
            rates.emplace_back(delta, i);
        }
    }
    std::sort(rates.rbegin(), rates.rend());
    std::string top;
    for (std::size_t i = 0; i < std::min<std::size_t>(rates.size(), 10); ++i) {
        top += fmt::format(" {:#04x}:{:.0f}", rates[i].second, rates[i].first / seconds);
    }
    LOG_INFO(Debug, "LabStats svc/s: total={:.0f}{}", total / seconds, top);

    const auto sleep = Snapshot(sleep_counts);
    const auto early = Snapshot(yield_early_outs);
    const auto switches = Snapshot(yield_switches);
    const auto rate = [seconds](u64 now_value, u64 then_value) { return (now_value - then_value) / seconds; };
    LOG_INFO(Debug, "LabStats sleep/s: ns>0={:.0f} 0={:.0f} -1={:.0f} -2={:.0f} other={:.0f}; "
                    "yield early-out/s 0={:.0f} -1={:.0f} -2={:.0f}; switched/s 0={:.0f} -1={:.0f} -2={:.0f}",
             rate(sleep[SleepPositive], s.sleep[SleepPositive]),
             rate(sleep[SleepYield0], s.sleep[SleepYield0]),
             rate(sleep[SleepYieldMinus1], s.sleep[SleepYieldMinus1]),
             rate(sleep[SleepYieldMinus2], s.sleep[SleepYieldMinus2]),
             rate(sleep[SleepOther], s.sleep[SleepOther]), rate(early[0], s.early[0]),
             rate(early[1], s.early[1]), rate(early[2], s.early[2]), rate(switches[0], s.switches[0]),
             rate(switches[1], s.switches[1]), rate(switches[2], s.switches[2]));

    s.svc = svc;
    s.sleep = sleep;
    s.early = early;
    s.switches = switches;
    s.intervals_ms.clear();
    s.window_start = now;
}

} // namespace Core::LabStats
