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

#include <chrono>
#include <cstring>
#include <string>
#include <x86intrin.h>

#include "core/arm/arm_interface.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nx2_probe.h"
#include "video_core/nx2_profile.h"

namespace Core::Nx2Probe {
namespace {


std::FILE* Out() {
    static std::FILE* const file = [] {
        const char* path = std::getenv("EDEN_NX2_PROBE_OUT");
        return path != nullptr ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

std::mutex out_mutex;

u64 Tsc() {
    return __rdtsc();
}

// Guest functions and return sites: open addressing on the guest address, lock-free (the JIT
// looks addresses up while probes add return sites).
constexpr u32 TableBits = 17;
constexpr u32 TableSize = 1u << TableBits;
constexpr u32 ReturnSite = 1u << 31;
std::atomic<u64> table_keys[TableSize];
std::atomic<u32> table_values[TableSize];

u32 TableHash(u64 pc) {
    return static_cast<u32>((pc * 0x9E3779B97F4A7C15ULL) >> (64 - TableBits));
}
u32 Lookup(u64 pc) {
    for (u32 i = TableHash(pc);; i = (i + 1) & (TableSize - 1)) {
        const u64 key = table_keys[i].load(std::memory_order_acquire);
        if (key == pc) {
            return table_values[i].load(std::memory_order_acquire);
        }
        if (key == 0) {
            return 0;
        }
    }
}
void Insert(u64 pc, u32 value) {
    for (u32 i = TableHash(pc);; i = (i + 1) & (TableSize - 1)) {
        u64 key = table_keys[i].load(std::memory_order_acquire);
        if (key == 0) {
            table_values[i].store(value, std::memory_order_release);
            if (table_keys[i].compare_exchange_strong(key, pc, std::memory_order_acq_rel)) {
                return;
            }
        }
        if (key == pc) {
            return;
        }
    }
}

constexpr u32 MaxSlots = 128;
constexpr u32 ThreadCount = 4096;

struct Cell {
    u64 calls, timed, inclusive, exclusive, svc, off, top_calls, top_inclusive;
};

struct Frame {
    u32 fn;
    u64 lr, sp, x0, wptr, cptr, entry, off_at, svc_at, probe_at, child;
};

struct GuestThread {
    u64 oncore, in_at, out_at, off, svc, probe;
    u32 slot = ~0u;
    bool seen;
    std::vector<Frame> stack;
};

struct Emitted {
    u64 headers, words;
};
struct Decoded {
    u64 calls, words, control_bytes, chunk_switches, unknown, abandoned;
};

struct Functions {
    std::vector<u64> pcs;
    std::vector<bool> command_buffer;
    std::vector<Cell> cells; // [fn * MaxSlots + slot]
    std::vector<u32> slot_threads;
    std::mutex slot_mutex;
    std::vector<GuestThread> threads{ThreadCount};
    std::mutex emitted_mutex;
    std::unordered_map<u64, Emitted> emitted; // fn << 16 | key
    std::vector<Decoded> decoded;
    u64 every = 300;
    bool timing = false;
};

Functions* LoadFunctions() {
    const char* path = std::getenv("EDEN_NX2_PC_COUNT");
    if (path == nullptr) {
        return nullptr;
    }
    auto* fns = new Functions;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        const u64 pc = std::strtoull(line.c_str(), nullptr, 16);
        if (pc == 0 || Lookup(pc) != 0) {
            continue;
        }
        fns->pcs.push_back(pc);
        fns->command_buffer.push_back(line.find(" c") != std::string::npos);
        Insert(pc, static_cast<u32>(fns->pcs.size())); // index + 1
    }
    fns->cells.resize(fns->pcs.size() * MaxSlots);
    fns->decoded.resize(fns->pcs.size());
    if (const char* every = std::getenv("EDEN_NX2_PC_COUNT_EVERY")) {
        fns->every = std::max<u64>(std::strtoull(every, nullptr, 10), 1);
    }
    fns->timing = std::getenv("EDEN_NX2_NVN_TIME") != nullptr;
    return fns;
}

Functions* GetFunctions() {
    static Functions* const fns = LoadFunctions();
    return fns;
}

u32 SlotOf(Functions& fns, GuestThread& thread, u32 thread_id) {
    if (thread.slot == ~0u) {
        std::scoped_lock lock{fns.slot_mutex};
        if (fns.slot_threads.size() < MaxSlots) {
            thread.slot = static_cast<u32>(fns.slot_threads.size());
            fns.slot_threads.push_back(thread_id);
        } else {
            thread.slot = MaxSlots - 1;
        }
    }
    return thread.slot;
}

bool ReadGuest(Core::Memory::Memory& memory, u64 address, void* out, size_t size) {
    auto* bytes = static_cast<u8*>(out);
    while (size != 0) {
        const size_t chunk = std::min<size_t>(size, 0x1000 - (address & 0xfff));
        const u8* const pointer = memory.GetPointerSilent(Common::ProcessAddress{address});
        if (pointer == nullptr) {
            return false;
        }
        std::memcpy(bytes, pointer, chunk);
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return true;
}

u64 Read64(Core::Memory::Memory& memory, u64 address) {
    u64 value{};
    return ReadGuest(memory, address, &value, sizeof(value)) ? value : 0;
}

void DecodeEmitted(Functions& fns, const Frame& frame, Core::Memory::Memory& memory) {
    const u64 wptr = Read64(memory, frame.x0);
    const u64 cptr = Read64(memory, frame.x0 + 0x28);
    std::scoped_lock lock{fns.emitted_mutex};
    Decoded& decoded = fns.decoded[frame.fn];
    if (wptr < frame.wptr || wptr - frame.wptr > 0x10000 || frame.wptr == 0) {
        ++decoded.chunk_switches;
        return;
    }
    ++decoded.calls;
    if (cptr >= frame.cptr && cptr - frame.cptr <= 0x10000) {
        decoded.control_bytes += cptr - frame.cptr;
    }
    const size_t count = static_cast<size_t>((wptr - frame.wptr) / 4);
    decoded.words += count;
    if (count == 0) {
        return;
    }
    static thread_local std::vector<u32> words;
    words.resize(count);
    if (!ReadGuest(memory, frame.wptr, words.data(), count * 4)) {
        return;
    }
    for (size_t i = 0; i < count;) {
        const u32 header = words[i];
        const u32 op = header >> 29;
        const u32 amount = (header >> 16) & 0x1fff;
        u32 data;
        if (op == 1 || op == 3 || op == 5) {
            data = amount;
        } else if (op == 4) {
            data = 0;
        } else {
            ++decoded.unknown;
            break;
        }
        Emitted& entry = fns.emitted[(u64{frame.fn} << 16) | (header & 0xffff)];
        ++entry.headers;
        entry.words += std::min<u64>(data, count - i - 1);
        i += 1 + data;
    }
}

void Complete(Functions& fns, GuestThread& thread, u64 now, Core::Memory::Memory& memory) {
    const Frame frame = thread.stack.back();
    thread.stack.pop_back();
    const u64 raw = now - frame.entry;
    const u64 off = thread.off - frame.off_at;
    const u64 probe = thread.probe - frame.probe_at;
    const u64 inclusive = raw > off + probe ? raw - off - probe : 0;
    Cell& cell = fns.cells[frame.fn * MaxSlots + thread.slot];
    ++cell.timed;
    cell.inclusive += inclusive;
    cell.exclusive += inclusive - std::min(frame.child, inclusive);
    cell.svc += thread.svc - frame.svc_at;
    cell.off += off;
    if (thread.stack.empty()) {
        ++cell.top_calls;
        cell.top_inclusive += inclusive;
        if (fns.command_buffer[frame.fn]) {
            DecodeEmitted(fns, frame, memory);
        }
    } else {
        thread.stack.back().child += inclusive;
    }
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

bool IsBlockProbe(u64 pc) {
    return Lookup(pc) != 0;
}

bool (*BlockProbeFunction())(u64) {
    return GetFunctions() != nullptr ? &IsBlockProbe : nullptr;
}

bool GuestTimingEnabled() {
    const Functions* fns = GetFunctions();
    return fns != nullptr && fns->timing;
}

bool OnBlockProbe(const GuestCall& call) {
    const u64 start = Tsc();
    Functions* const fns_pointer = GetFunctions();
    if (fns_pointer == nullptr) {
        return false;
    }
    Functions& fns = *fns_pointer;
    GuestThread& thread = fns.threads[call.thread_id % ThreadCount];
    const u32 value = Lookup(call.pc);
    bool halt = false;
    if ((value & ReturnSite) == 0 && value != 0) {
        const u32 fn = value - 1;
        const u32 slot = SlotOf(fns, thread, call.thread_id);
        ++fns.cells[fn * MaxSlots + slot].calls;
        if (fns.timing) {
            if (Lookup(call.lr) == 0) {
                Insert(call.lr, ReturnSite);
                for (size_t core = 0; core < 4; ++core) {
                    if (auto* arm = call.process->GetArmInterface(core)) {
                        arm->InvalidateCacheRange(call.lr, 4);
                    }
                }
                halt = true;
            }
            Frame frame{.fn = fn, .lr = call.lr, .sp = call.sp, .x0 = call.x0};
            if (fns.command_buffer[fn]) {
                frame.wptr = Read64(*call.memory, call.x0);
                frame.cptr = Read64(*call.memory, call.x0 + 0x28);
            }
            thread.stack.push_back(frame);
            const u64 end = Tsc();
            thread.probe += end - start;
            Frame& pushed = thread.stack.back();
            pushed.entry = end;
            pushed.probe_at = thread.probe;
            pushed.off_at = thread.off;
            pushed.svc_at = thread.svc;
            return halt;
        }
    } else if ((value & ReturnSite) != 0 && fns.timing && !thread.stack.empty()) {
        size_t match = thread.stack.size();
        while (match != 0 &&
               !(thread.stack[match - 1].lr == call.pc && thread.stack[match - 1].sp == call.sp)) {
            --match;
        }
        if (match != 0) {
            if (match < thread.stack.size()) {
                std::scoped_lock lock{fns.emitted_mutex};
                fns.decoded[thread.stack.back().fn].abandoned += thread.stack.size() - match;
            }
            thread.stack.resize(match);
            // A tail call leaves its caller's frame under it with the same return: both end here.
            while (!thread.stack.empty() && thread.stack.back().lr == call.pc &&
                   thread.stack.back().sp == call.sp) {
                Complete(fns, thread, start, *call.memory);
            }
        }
    }
    thread.probe += Tsc() - start;
    return halt;
}

void OnGuestSwitch(u32 previous, u32 next) {
    Functions* const fns = GetFunctions();
    if (fns == nullptr || !fns->timing) {
        return;
    }
    const u64 now = Tsc();
    if (previous != ~0u) {
        GuestThread& thread = fns->threads[previous % ThreadCount];
        if (thread.in_at != 0) {
            thread.oncore += now - thread.in_at;
            thread.in_at = 0;
        }
        thread.out_at = now;
    }
    if (next != ~0u) {
        GuestThread& thread = fns->threads[next % ThreadCount];
        if (thread.out_at != 0) {
            thread.off += now - thread.out_at;
            thread.out_at = 0;
        }
        thread.in_at = now;
        thread.seen = true;
    }
}

void OnSvc(u32 thread_id, u64 start_tsc, u64 end_tsc) {
    Functions* const fns = GetFunctions();
    if (fns != nullptr) {
        fns->threads[thread_id % ThreadCount].svc += end_tsc - start_tsc;
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

bool OnPresentWanted() {
    return PaceTraceEnabled() || GetFunctions() != nullptr || VideoCommon::OriginProfile::g_enabled;
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
    if (Functions* fns = GetFunctions(); fns != nullptr && timing.present % fns->every == 0) {
        text += fmt::format("t {} {} {}\n", timing.present, Tsc(),
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
        for (size_t fn = 0; fn < fns->pcs.size(); ++fn) {
            for (u32 slot = 0; slot < fns->slot_threads.size(); ++slot) {
                const Cell& c = fns->cells[fn * MaxSlots + slot];
                if (c.calls != 0) {
                    text += fmt::format("g {} {:#x} {} {} {} {} {} {} {} {} {}\n", timing.present,
                                        fns->pcs[fn], fns->slot_threads[slot], c.calls, c.timed,
                                        c.inclusive, c.exclusive, c.svc, c.off, c.top_calls,
                                        c.top_inclusive);
                }
            }
        }
        {
            std::scoped_lock lock{fns->emitted_mutex};
            for (const auto& [key, e] : fns->emitted) {
                text += fmt::format("e {} {:#x} {:#x} {} {}\n", timing.present,
                                    fns->pcs[key >> 16], key & 0xffff, e.headers, e.words);
            }
            for (size_t fn = 0; fn < fns->pcs.size(); ++fn) {
                const Decoded& d = fns->decoded[fn];
                if (d.calls != 0 || d.chunk_switches != 0 || d.abandoned != 0) {
                    text += fmt::format("ed {} {:#x} {} {} {} {} {} {}\n", timing.present,
                                        fns->pcs[fn], d.calls, d.words, d.control_bytes,
                                        d.chunk_switches, d.unknown, d.abandoned);
                }
            }
        }
        for (u32 tid = 0; tid < ThreadCount; ++tid) {
            const GuestThread& t = fns->threads[tid];
            if (t.seen || t.probe != 0) {
                text += fmt::format("r {} {} {} {} {} {}\n", timing.present, tid, t.oncore, t.off,
                                    t.svc, t.probe);
            }
        }
    }
    if (VideoCommon::OriginProfile::g_enabled && timing.present % 300 == 0) {
        if (GetFunctions() == nullptr) {
            text += fmt::format("t {} {} {}\n", timing.present, Tsc(),
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
        }
        VideoCommon::OriginProfile::Dump(text, timing.present);
    }
    std::scoped_lock lock{out_mutex};
    std::fwrite(text.data(), 1, text.size(), out);
    std::fflush(out);
}

} // namespace Core::Nx2Probe
