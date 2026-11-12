// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// EDEN_NX2_IDLE_TRACE=<file>: a timestamped binary trace of the split GPU threads (the front's
// drains, waits, suspended channels and polls, the back's idle time and sleeps, the command list
// copies' lifecycle, presents), to tell what the front did while the back slept. Each thread
// fills its own chunks; a writer thread appends the full ones to the file, with time stamp
// counter calibrations. tools/nx2/idle_trace.py reads it.
namespace VideoCommon::IdleTrace {

enum class Event : u16 {
    // GPUBack
    BackIdle,  ///< Ran out of work (arg: its ring position, low 32 bits)
    BackSleep, ///< ...sleeps in its futex
    BackWake,  ///< ...woke up
    BackRun,   ///< Runs entries again (arg: words published to it)
    HookBegin, ///< The idle hook (deferred submit); aux 0: before sleeping, 1: between entries
    HookEnd,
    ListCopyFlush, ///< Command list page copies submitted (arg: tick); aux 0: after a dispatch,
                   ///< 1: after a DMA copy
    // VulkanWorker
    WorkerSubmitBegin, ///< arg: the submission's tick
    WorkerSubmitEnd,
    // GPU thread (the split's front)
    QueueWaitBegin, ///< Waits for the CPU's commands with no channel pending
    QueueWaitEnd,
    PushBegin, ///< Parses a submitted command list (aux: channel)
    PushEnd,
    PollSleepBegin, ///< Pending channels' poll loop rests; aux 0 yield, 1 50 us, 2 1 ms
    PollSleepEnd,
    ResumeBegin, ///< A pending channel resumed and runs (aux: channel)
    ResumeEnd,
    DrainBegin, ///< Drain (aux: DrainSite), only when it waits
    DrainEnd,
    WaitForBegin, ///< WaitFor or the ring's room (aux: DrainSite)
    WaitForEnd,
    AcquireDrain,   ///< An acquire not met without a drain drains (aux: channel, arg: address)
    AcquireSuspend, ///< The channel waits on the acquire (aux: channel, arg: address)
    AcquirePoll,    ///< A poll of it (aux: channel, arg: AcquirePollResult)
    AcquireResume,  ///< aux: channel
    ListSuspend,    ///< The channel waits on a command list copy (aux: channel, arg: tick)
    ListPoll,       ///< A poll of it (aux: channel, arg: 1 done)
    ListResume,     ///< aux: channel
    Publish,        ///< Words published to the back; aux 1: the back was waiting (woken)
    EarlyPublish,   ///< ...early, as the back waits for them (Commit)
    SettleIdle,     ///< The poll loop asked an idle back for its hook (SettleIdleBack)
    HookRequest,    ///< RequestIdleHook (any thread)
    // Other threads
    Present,     ///< RendererVulkan::Composite
    GuestSubmit, ///< The CPU submits a command list (aux: channel)
    // GPU timestamps of a submission (arg: its tick), written by VulkanWorker once the GPU is
    // done with it. Their time is the GPU's clock in ns, not the time stamp counter.
    GpuSubmitStart, ///< Top of pipe at the start of the submission
    GpuProducer,    ///< Bottom of pipe before the dispatch that writes command list pages
    GpuCopy,        ///< Bottom of pipe before the command list page copies
    GpuSubmitEnd,   ///< Bottom of pipe at the end of the submission
    // dotfiles-2hg.58 diagnostics (CPU cores and the back)
    GuestSwitch, ///< A core switched guest threads (aux: core | 0x100 idle thread, arg: thread id)
    SvcEnter,    ///< aux: SVC number, arg: guest thread id
    SvcExit,     ///< aux: SVC number, arg: guest thread id
    HoldBegin,   ///< HoldHleReply starts holding (arg: client thread id)
    HoldEnd,     ///< aux: 0 client running again, 1 core idle, 2 budget used up
    SchedFlush,  ///< Scheduler::Flush/Finish (aux: caller file id | 0x8000 Finish, arg: line)
    StorageSyncBegin, ///< Storage buffer bind (aux: 1 compute | 2 multi-range, arg: bytes)
    StorageSyncEnd,   ///< ...synchronized (aux: 4 MiB tracker managers the range spans)
    ExclCount,   ///< Per present: aux core << 8 | kind, arg: cumulative count (low 32 bits)
};

/// SchedFlush's file id for a source file name (logged once per new name).
u16 FileId(const char* file_name);

enum class AcquirePollResult : u32 {
    Met,
    MetByQueued,
    Latched,
    Waiting,       ///< Not met, no drain
    DrainedMet,    ///< Met (or latched) after a drain
    DrainedWaiting,
};

/// Whether the trace records (a plain flag: checked on the GPU threads' hot paths).
inline bool g_enabled{};

/// Called by VideoCore::CreateGPU: opens EDEN_NX2_IDLE_TRACE's file once.
void ReadSwitch();

void Write(Event event, u16 aux, u32 arg);
/// A record with its own time (the Gpu* events).
void WriteAt(u64 time, Event event, u16 aux, u32 arg);

inline void Note(Event event, u32 aux = 0, u32 arg = 0) {
    if (g_enabled) {
        Write(event, static_cast<u16>(aux), arg);
    }
}

} // namespace VideoCommon::IdleTrace
