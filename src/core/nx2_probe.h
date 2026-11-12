// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostic probes (branch nvn-probe, dotfiles-2hg.62; not for merging). All write to the file
// EDEN_NX2_PROBE_OUT names, and are off unless their variable is set:
// - EDEN_NX2_PC_COUNT=<file>: one guest function per line, "<hex address> [c]" ("c": its first
//   argument is an NVN command buffer). JIT blocks starting there call OnBlockProbe. Cumulative
//   per function and guest thread every EDEN_NX2_PC_COUNT_EVERY presents (default 300):
//   "g <present> <pc> <thread> <calls> <inclusive> <exclusive> <svc> <off core> <top calls>
//   <top inclusive>" (times in TSC ticks; "t <present> <tsc> <steady ns>" converts them).
// - EDEN_NX2_NVN_TIME=1 (with EDEN_NX2_PC_COUNT): also times each call from entry to its return
//   (the return address becomes a probe too), without the time the guest thread was off its core
//   or in the probes, and decodes the command words a top-level call on a command buffer wrote:
//   "e <present> <pc> <subchannel << 13 | method> <headers> <data words>", "ed <present> <pc>
//   <decoded calls> <words> <control bytes> <chunk switches> <unknown opcodes>". Per guest
//   thread "r <present> <thread> <on core> <off core> <svc> <probe>".
// - EDEN_NX2_PACE_TRACE=1: per present "p <present> <thread> <entered ns> <fence us> <composite
//   us> <pace us> <pace requested us>", and per present the guest waits that ended since the
//   last one, by thread and kind: "w <present> <thread> <kind> <count> <us>".

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}
namespace Kernel {
class KProcess;
}

namespace Core::Nx2Probe {

/// Whether a JIT block starting at `pc` calls OnBlockProbe (dynarmic's block_entry_probe).
bool IsBlockProbe(u64 pc);
/// Null when EDEN_NX2_PC_COUNT is unset.
bool (*BlockProbeFunction())(u64);

struct GuestCall {
    u64 pc, x0, lr, sp;
    u32 thread_id;
    Core::Memory::Memory* memory;
    Kernel::KProcess* process;
};
/// Returns whether the calling core's JIT must halt (its code cache was invalidated).
bool OnBlockProbe(const GuestCall& call);

/// Guest scheduler: `previous`/`next` thread ids, ~0 for the idle thread.
void OnGuestSwitch(u32 previous, u32 next);
bool GuestTimingEnabled();
void OnSvc(u32 thread_id, u64 start_tsc, u64 end_tsc);

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
bool OnPresentWanted();

} // namespace Core::Nx2Probe
