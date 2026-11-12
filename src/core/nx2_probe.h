// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostic probes (branch nvn-probe, dotfiles-2hg.62; not for merging). All write to the file
// EDEN_NX2_PROBE_OUT names, and are off unless their variable is set:
// - EDEN_NX2_PC_COUNT=<file of hex guest addresses>: counts JIT block entries at those addresses
//   per guest thread; cumulative counts every EDEN_NX2_PC_COUNT_EVERY presents (default 300) as
//   "c <present> <pc> <thread> <count>".
// - EDEN_NX2_PACE_TRACE=1: per present "p <present> <thread> <entered ns> <fence us> <composite
//   us> <pace us> <pace requested us>", and per present the guest waits that ended since the
//   last one, by thread and kind: "w <present> <thread> <kind> <count> <us>".

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

#include "common/common_types.h"

namespace Core::Nx2Probe {

/// Guest addresses whose block entries are counted; null when EDEN_NX2_PC_COUNT is unset.
const std::unordered_set<u64>* BlockEntryProbes();
void CountBlockEntry(u64 pc, u32 thread_id);

bool PaceTraceEnabled();
/// An HLE service handled a request from this client thread.
void NoteRequest(u32 client_thread_id, std::string_view service, u32 command);
/// The service request kind last noted for this thread ("?" if none since the last call).
std::string TakeRequest(u32 thread_id);
void NoteWait(u32 thread_id, const std::string& kind, u64 ns);

struct PresentTiming {
    u64 present;
    u32 thread_id;
    s64 entered_ns;
    s64 fence_us, composite_us, pace_us, pace_requested_us;
};
void OnPresent(const PresentTiming& timing);

} // namespace Core::Nx2Probe
