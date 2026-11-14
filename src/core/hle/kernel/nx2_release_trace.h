// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Lab-only (dotfiles-vvt.10): EDEN_NX2_RELEASE_TRACE=<device address> traps the guest CPU's writes
// to the semaphore words in that device page (the page goes to the slow write path, as GPU-cached
// memory does) and, for each, names the writing guest thread, the wait it came out of (SVC, the
// arbiter address, what woke it: a guest thread or a host thread) and the same for that waker and
// its waker, with their times: wait start -> wake -> on a core -> write. The GPU front adds how
// long after the write it saw the word (FrontSeen). Logged every 10 s.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Kernel::Nx2ReleaseTrace {

inline const u64 g_page = [] {
    const char* const value = std::getenv("EDEN_NX2_RELEASE_TRACE");
    return value != nullptr ? std::strtoull(value, nullptr, 0) & ~u64{0xFFF} : u64{0};
}();
inline const bool g_enabled = g_page != 0;

[[nodiscard]] inline s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Who woke a thread: a guest thread id, or the host thread's name.
struct Waker {
    u64 tid{};               ///< Guest thread, 0 when a host thread did
    std::array<char, 16> host{}; ///< Host thread name otherwise
};

[[nodiscard]] inline std::string Label(const Waker& waker) {
    if (waker.tid != 0) {
        return fmt::format("t{}", waker.tid);
    }
    return fmt::format("host:{}", waker.host.data());
}

[[nodiscard]] inline Waker HostWaker() {
    Waker waker{};
#if defined(__linux__)
    pthread_getname_np(pthread_self(), waker.host.data(), waker.host.size());
#else
    std::strcpy(waker.host.data(), "?");
#endif
    return waker;
}

/// A thread's latest wait, as it ended.
struct Wait {
    u64 tid{};
    u32 svc{};
    u64 address{}; ///< WaitForAddress' address, else 0
    s64 start{};   ///< Entered the wait
    s64 wake{};    ///< Made runnable
    s64 run{};     ///< Next on a core
    Waker waker{};
};

/// A thread's latest wait and, as of its wake, its waker's and that one's.
struct Chain {
    std::array<Wait, 3> links{};
    u32 pending_svc{};
    u64 pending_address{};
    s64 pending_start{};
};

struct Tally {
    u64 count{};
    std::array<double, 9> sum{}; ///< ms: see Report
};

struct State {
    std::mutex mutex;
    std::unordered_map<u64, Chain> threads;
    std::map<std::string, Tally> chains;
    std::array<s64, 1024> last_write{}; ///< ns, by word in the page
    double seen_sum{};
    u64 seen_count{};
    s64 last_report{};
};

inline State& Get() {
    static State state;
    return state;
}

/// The thread is about to wait in `svc` (for WaitForAddress, on `address`).
inline void OnSvcWait(u64 tid, u32 svc, u64 address) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Chain& chain = s.threads[tid];
    chain.pending_svc = svc;
    chain.pending_address = address;
}

inline void OnWaitBegin(u64 tid) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.threads[tid].pending_start = Now();
}

/// `tid` was made runnable by `waker` (and, when a guest thread, its own chain).
inline void OnWake(u64 tid, const Waker& waker) {
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Chain& chain = s.threads[tid];
    Wait own{tid, chain.pending_svc, chain.pending_address, chain.pending_start, now, 0, waker};
    std::array<Wait, 3> links{};
    links[0] = own;
    if (waker.tid != 0) {
        if (const auto it = s.threads.find(waker.tid); it != s.threads.end()) {
            links[1] = it->second.links[0];
            links[2] = it->second.links[1];
        }
    }
    chain.links = links;
}

inline void OnRun(u64 tid) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (const auto it = s.threads.find(tid); it != s.threads.end() && it->second.links[0].run == 0) {
        it->second.links[0].run = Now();
    }
}

inline void Report(State& s, s64 now) {
    if (now - s.last_report < 10'000'000'000LL) {
        return;
    }
    s.last_report = now;
    std::vector<std::pair<u64, const std::string*>> order;
    for (const auto& [key, tally] : s.chains) {
        order.emplace_back(tally.count, &key);
    }
    std::ranges::sort(order, std::greater{});
    LOG_INFO(Kernel,
             "Release trace in 10 s: front saw the words {:.3f} ms after the write on average "
             "({} sightings); chains (writer <- waker <- its waker), count, mean ms: writer "
             "waited / woke->ran / ran->write, waker waited / woke->ran / ran->woke writer, its "
             "waker likewise",
             s.seen_count ? s.seen_sum / static_cast<double>(s.seen_count) : 0.0, s.seen_count);
    for (size_t i = 0; i < (std::min)(order.size(), size_t{8}); ++i) {
        const Tally& t = s.chains[*order[i].second];
        std::string means;
        for (const double sum : t.sum) {
            means += fmt::format(" {:.3f}", sum / static_cast<double>(t.count));
        }
        LOG_INFO(Kernel, "  {} x{}:{}", *order[i].second, t.count, means);
    }
    s.chains.clear();
    s.seen_sum = 0;
    s.seen_count = 0;
}

/// The guest thread `tid` wrote `size` bytes at device address `address` in the traced page.
inline void OnWrite(u64 tid, u64 address) {
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.last_write[((address - g_page) / 4) % s.last_write.size()] = now;
    const auto it = s.threads.find(tid);
    if (it == s.threads.end()) {
        return;
    }
    const auto& links = it->second.links;
    const auto link_text = [](const Wait& wait) {
        if (wait.tid == 0) {
            return std::string{"?"};
        }
        return fmt::format("t{}[svc{:#x}{}]", wait.tid, wait.svc,
                           wait.address != 0 ? fmt::format(" @{:#x}", wait.address)
                                             : std::string{});
    };
    const std::string key =
        fmt::format("@{:#x} {} <- {} <- {} <- {}", address, link_text(links[0]),
                    Label(links[0].waker), links[1].tid ? Label(links[1].waker) : "-",
                    links[2].tid ? Label(links[2].waker) : "-");
    Tally& tally = s.chains[key];
    ++tally.count;
    const auto ms = [](s64 from, s64 to) {
        return from != 0 && to >= from ? static_cast<double>(to - from) / 1e6 : 0.0;
    };
    tally.sum[0] += ms(links[0].start, links[0].wake);
    tally.sum[1] += ms(links[0].wake, links[0].run);
    tally.sum[2] += ms(links[0].run, now);
    tally.sum[3] += ms(links[1].start, links[1].wake);
    tally.sum[4] += ms(links[1].wake, links[1].run);
    tally.sum[5] += ms(links[1].run, links[0].wake);
    tally.sum[6] += ms(links[2].start, links[2].wake);
    tally.sum[7] += ms(links[2].wake, links[2].run);
    tally.sum[8] += ms(links[2].run, links[1].wake);
    Report(s, now);
}

/// The GPU front found the word at `address` (device) met by memory.
inline void FrontSeen(u64 address) {
    if ((address & ~u64{0xFFF}) != g_page) {
        return;
    }
    const s64 now = Now();
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    const s64 written = s.last_write[((address - g_page) / 4) % s.last_write.size()];
    if (written != 0 && now >= written) {
        s.seen_sum += static_cast<double>(now - written) / 1e6;
        ++s.seen_count;
    }
}

} // namespace Kernel::Nx2ReleaseTrace
