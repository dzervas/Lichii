#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the split GPU thread's front did while its back was idle, from an EDEN_NX2_IDLE_TRACE file.

  idle_trace.py <trace> [--marks marks.json | --presents FIRST:LAST | --last-seconds S] [--json OUT]

The window: presents FIRST..LAST (1-based, counted from the trace's first present; bench.py's
marks.json "measure" and "end" when given), or the last S seconds both GPU threads traced
(default 40). Printed: the back's idle time (ran out of work until it runs entries again) and
its futex sleeps per frame, attributed to what the front was in at the time; latency
distributions of the command list copy round trip and of the acquires the channels wait on; and
event counts per frame. With GPU timestamps in the trace (Gpu* events), the copies' submissions
are broken down on the GPU's clock as well (gpu_list_breakdown).

Front states, by priority: drain:<site> and waitfor:<site> (it waits for the back: the back runs
its idle hook), poll:<reason> (only suspended channels, polled; reason: the suspensions, `list`
for command list copies, `acquire-cpu` for acquires met by the word in memory, `acquire-back`
for those a release the back ran latched, `acquire-drained` for those met after a drain),
queue-empty (no command from the CPU), work:after-<last wait> (parsing and running macros, by
what the front last waited on). The summary maps them to the causes (a) guest-CPU release wait,
(b) command list copy round trip, (c) refill after a drain or wait, (d) other.

Standard library only (the decoding takes ~1 s per million events).
"""

import argparse
import array
import bisect
import collections
import json
import struct
import sys

MAGIC = 0x544C4449
CALIBRATION = 0xFFFFFFFF

EVENTS = [
    "BackIdle", "BackSleep", "BackWake", "BackRun", "HookBegin", "HookEnd", "ListCopyFlush",
    "WorkerSubmitBegin", "WorkerSubmitEnd",
    "QueueWaitBegin", "QueueWaitEnd", "PushBegin", "PushEnd", "PollSleepBegin", "PollSleepEnd",
    "ResumeBegin", "ResumeEnd", "DrainBegin", "DrainEnd", "WaitForBegin", "WaitForEnd",
    "AcquireDrain", "AcquireSuspend", "AcquirePoll", "AcquireResume", "ListSuspend", "ListPoll",
    "ListResume", "Publish", "EarlyPublish", "SettleIdle", "HookRequest",
    "Present", "GuestSubmit",
    "GpuSubmitStart", "GpuProducer", "GpuCopy", "GpuSubmitEnd",
    "GuestSwitch", "SvcEnter", "SvcExit", "HoldBegin", "HoldEnd", "SchedFlush",
    "StorageSyncBegin", "StorageSyncEnd", "ExclCount",
]
GPU_EVENTS = ("GpuSubmitStart", "GpuProducer", "GpuCopy", "GpuSubmitEnd")
E = {name: index for index, name in enumerate(EVENTS)}
POLL_RESULTS = ["met", "met-by-queued", "latched", "waiting", "drained-met", "drained-waiting"]
# gpu_split.h DrainSite
SITES = ["other", "fetch drain", "acquire", "semaphore flush", "fence release", "list writers",
         "list read writers", "list sync", "list flush", "macro dma read", "macro dma writes",
         "macro dma flush", "gp sync fence", "map", "front start", "front verify", "ring full",
         "back register", "back code"]


def site_name(aux):
    return SITES[aux] if aux < len(SITES) else f"site{aux}"


class Thread:
    def __init__(self, index):
        self.index = index
        self.name = ""
        self.ticks = array.array("Q")
        self.codes = array.array("I")  # event | aux << 16
        self.args = array.array("I")

    def count(self, event):
        return sum(1 for code in self.codes if code & 0xFFFF == E[event])


def read(path):
    threads, calibrations = {}, []
    record = struct.Struct("<QIHH")
    with open(path, "rb") as handle:
        data = handle.read()
    offset, size = 0, len(data)
    while offset + 16 <= size:
        magic, thread, count, _ = struct.unpack_from("<IIII", data, offset)
        if magic != MAGIC:
            sys.exit(f"bad block at {offset:#x}")
        offset += 16
        if thread == CALIBRATION:
            if offset + 16 > size:
                break
            calibrations.append(struct.unpack_from("<QQ", data, offset))
            offset += 16
            continue
        entry = threads.setdefault(thread, Thread(thread))
        if count == 0:
            entry.name = data[offset:offset + 16].split(b"\0")[0].decode(errors="replace")
            offset += 16
            continue
        end = offset + count * 16
        if end > size:
            break  # cut short by the process's end
        for ticks, arg, event, aux in record.iter_unpack(data[offset:end]):
            entry.ticks.append(ticks)
            entry.codes.append(event | aux << 16)
            entry.args.append(arg)
        offset = end
    # Blocks of a thread arrive in order; threads interleave, each in its own list.
    return threads, calibrations


def clock(calibrations):
    """Ticks -> ns (steady clock), from the first and last calibration."""
    (t0, n0), (t1, n1) = calibrations[0], calibrations[-1]
    scale = (n1 - n0) / (t1 - t0) if t1 != t0 else 1.0
    return lambda ticks: n0 + (ticks - t0) * scale, scale


def role(threads, event):
    best = max(threads.values(), key=lambda t: t.count(event))
    return best if best.count(event) else None


def percentiles(values):
    if not values:
        return "n=0"
    values = sorted(values)
    pick = lambda q: values[min(len(values) - 1, int(q * len(values)))]
    mean = sum(values) / len(values)
    return (f"n={len(values)} mean {mean:.1f} p50 {pick(0.5):.1f} p90 {pick(0.9):.1f} "
            f"p99 {pick(0.99):.1f} max {values[-1]:.1f} us")


def back_intervals(back, begin, end):
    """The back's idle, sleep and hook intervals (ticks) within [begin, end)."""
    idle, sleep, hooks = [], [], []
    idle_start = sleep_start = hook_start = None
    for ticks, code in zip(back.ticks, back.codes):
        event = code & 0xFFFF
        if event == E["BackIdle"]:
            if idle_start is None:
                idle_start = ticks
        elif event == E["BackRun"]:
            if idle_start is not None:
                idle.append((idle_start, ticks))
                idle_start = None
        elif event == E["BackSleep"]:
            sleep_start = ticks
        elif event == E["BackWake"]:
            if sleep_start is not None:
                sleep.append((sleep_start, ticks))
                sleep_start = None
        elif event == E["HookBegin"]:
            hook_start = ticks
        elif event == E["HookEnd"] and hook_start is not None:
            hooks.append((hook_start, ticks, code >> 16))
            hook_start = None
    clip = lambda spans: [(max(a, begin), min(b, end)) + tuple(rest)
                          for a, b, *rest in spans if b > begin and a < end]
    return clip(idle), clip(sleep), clip(hooks)


def suspensions(front):
    """Pass 1: every acquire and list suspension with its lifecycle (ticks)."""
    acquires, lists = [], []
    open_acquire, open_list = {}, {}
    last_drain = {}  # channel -> (AcquireDrain ticks, address)
    drain_end = None
    for ticks, code, arg in zip(front.ticks, front.codes, front.args):
        event, aux = code & 0xFFFF, code >> 16
        if event == E["AcquireDrain"]:
            last_drain[aux] = (ticks, arg)
            drain_end = None
        elif event == E["DrainEnd"] and aux == SITES.index("acquire") and drain_end is None:
            drain_end = ticks
        elif event == E["AcquireSuspend"]:
            drained = last_drain.get(aux)
            open_acquire[aux] = {
                "channel": aux, "address": arg, "suspend": ticks,
                "drain": drained[0] if drained and drained[1] == arg else None,
                "drain_end": drain_end, "polls": 0, "last_waiting": None, "seen": None,
                "result": None, "resume": None}
        elif event == E["AcquirePoll"]:
            entry = open_acquire.get(aux)
            if entry is None:
                continue
            entry["polls"] += 1
            result = POLL_RESULTS[arg] if arg < len(POLL_RESULTS) else str(arg)
            if result in ("waiting", "drained-waiting"):
                entry["last_waiting"] = ticks
            elif entry["seen"] is None:
                entry["seen"], entry["result"] = ticks, result
        elif event == E["AcquireResume"]:
            entry = open_acquire.pop(aux, None)
            if entry is not None:
                entry["resume"] = ticks
                acquires.append(entry)
        elif event == E["ListSuspend"]:
            open_list[aux] = {"channel": aux, "tick": arg, "suspend": ticks, "last_false": None,
                              "first_true": None, "resume": None, "polls": 0}
        elif event == E["ListPoll"]:
            entry = open_list.get(aux)
            if entry is None:
                continue
            entry["polls"] += 1
            if arg:
                if entry["first_true"] is None:
                    entry["first_true"] = ticks
            else:
                entry["last_false"] = ticks
        elif event == E["ListResume"]:
            entry = open_list.pop(aux, None)
            if entry is not None:
                entry["resume"] = ticks
                lists.append(entry)
    return acquires, lists


def acquire_class(entry):
    result = entry["result"]
    if result == "met":
        return "acquire-cpu"
    if result in ("latched", "met-by-queued"):
        return "acquire-back"
    return "acquire-drained"


def front_segments(front, acquire_classes):
    """Pass 2: the front's state as [start, end, label] segments (ticks)."""
    segments = []
    pending = {}  # channel -> reason
    waits = []    # drain/waitfor stack: labels
    in_poll_sleep = in_queue = False
    work_depth = 0  # inside Push or a resumed channel's run
    after = "start"
    last = None

    def label():
        if waits:
            return waits[-1]
        if pending and (in_poll_sleep or work_depth == 0):
            reasons = sorted(set(pending.values()))
            return "poll:" + "+".join(reasons)
        if in_queue:
            return "queue-empty"
        return "work:after-" + after

    for ticks, code, arg in zip(front.ticks, front.codes, front.args):
        event, aux = code & 0xFFFF, code >> 16
        current = label()
        if last is not None and ticks > last:
            if segments and segments[-1][2] == current and segments[-1][1] == last:
                segments[-1][1] = ticks
            else:
                segments.append([last, ticks, current])
        last = ticks
        if event == E["DrainBegin"]:
            waits.append("drain:" + site_name(aux))
        elif event == E["WaitForBegin"]:
            waits.append("waitfor:" + site_name(aux))
        elif event in (E["DrainEnd"], E["WaitForEnd"]):
            if waits:
                after = waits.pop()
        elif event == E["PollSleepBegin"]:
            in_poll_sleep = True
        elif event == E["PollSleepEnd"]:
            in_poll_sleep = False
        elif event == E["QueueWaitBegin"]:
            in_queue = True
        elif event == E["QueueWaitEnd"]:
            in_queue = False
            after = "queue"
        elif event in (E["PushBegin"], E["ResumeBegin"]):
            work_depth += 1
        elif event in (E["PushEnd"], E["ResumeEnd"]):
            work_depth = max(0, work_depth - 1)
        elif event == E["AcquireSuspend"]:
            pending[aux] = acquire_classes.get((aux, ticks), "acquire-unresolved")
        elif event == E["AcquireResume"]:
            if aux in pending:
                after = "resume:" + pending.pop(aux)
        elif event == E["ListSuspend"]:
            pending[aux] = "list"
        elif event == E["ListResume"]:
            if pending.pop(aux, None) is not None:
                after = "resume:list"
    return segments


def transitions(front, acquire_classes):
    """The front events that end a wait of its own, as (ticks, label), sorted."""
    result = []
    pending = {}
    for ticks, code in zip(front.ticks, front.codes):
        event, aux = code & 0xFFFF, code >> 16
        if event == E["AcquireSuspend"]:
            pending[aux] = acquire_classes.get((aux, ticks), "acquire-unresolved")
        elif event == E["AcquireResume"]:
            result.append((ticks, "resume:" + pending.pop(aux, "acquire-unresolved")))
        elif event == E["ListResume"]:
            result.append((ticks, "resume:list"))
        elif event == E["QueueWaitEnd"]:
            result.append((ticks, "command from the CPU"))
        elif event == E["PushBegin"]:
            # Popped without a wait while channels are suspended (the poll loop's TryPop).
            if result and result[-1][1] != "command from the CPU":
                result.append((ticks, "command from the CPU, channels suspended"))
        elif event in (E["DrainEnd"], E["WaitForEnd"]):
            result.append((ticks, ("drain:" if event == E["DrainEnd"] else "waitfor:")
                           + site_name(aux)))
    return result


def ended_by(idle, sleep, front_transitions, segments):
    """Each idle span (and the futex sleep within it) by the front's last own wait that ended
    before the back got work (the span's end), when it ended within the span; else the front
    was busy all along (by its state when the span began). Sleeps also end at idle hook requests, without work: only idle spans
    end with work."""
    times = [t for t, _ in front_transitions]
    sleep_starts = [a for a, _ in sleep]
    segment_starts = [segment[0] for segment in segments]
    idle_totals, sleep_totals, counts = (collections.Counter() for _ in range(3))
    for begin, end, *_ in idle:
        i = bisect.bisect_right(times, end) - 1
        if i >= 0 and times[i] >= begin:
            label = front_transitions[i][1]
        else:
            k = bisect.bisect_right(segment_starts, begin) - 1
            label = "no wait ended, front in " + (segments[k][2] if k >= 0 else "?")
        idle_totals[label] += end - begin
        counts[label] += 1
        j = bisect.bisect_left(sleep_starts, begin)
        while j < len(sleep) and sleep[j][0] < end:
            sleep_totals[label] += min(sleep[j][1], end) - sleep[j][0]
            j += 1
    return idle_totals, sleep_totals, counts


def attribute(spans, segments):
    """Ticks of the spans by the segment labels overlapping them."""
    totals = collections.Counter()
    starts = [segment[0] for segment in segments]
    for begin, end, *_ in spans:
        index = max(0, bisect.bisect_right(starts, begin) - 1)
        cursor = begin
        while cursor < end and index < len(segments):
            seg_begin, seg_end, name = segments[index]
            if seg_end <= cursor:
                index += 1
                continue
            if seg_begin > cursor:
                totals["untraced"] += min(seg_begin, end) - cursor
                cursor = min(seg_begin, end)
                continue
            piece = min(seg_end, end) - cursor
            totals[name] += piece
            cursor += piece
            index += 1
        if cursor < end:
            totals["untraced"] += end - cursor
    return totals


def cause(label):
    if label.startswith("poll:"):
        reasons = label[5:].split("+")
        if reasons == ["list"]:
            return "(b) list copy round trip"
        if all(r == "acquire-cpu" for r in reasons):
            return "(a) guest-CPU release wait"
        if "list" in reasons:
            return "(d) other: poll mixed list+acquire"
        return "(d) other: poll on back-released/drained acquires"
    if label.startswith("work:after-resume:"):
        what = label[len("work:after-resume:"):]
        if what == "list":
            return "(b) refill after list resume"
        if what == "acquire-cpu":
            return "(a) refill after guest release"
        return "(d) refill after other acquire resume"
    if label.startswith("work:after-drain") or label.startswith("work:after-waitfor"):
        return "(c) post-drain/wait refill"
    if label.startswith("drain:") or label.startswith("waitfor:"):
        return "(d) front waits for back (hook)"
    if label == "queue-empty":
        return "(d) no CPU command queued"
    return "(d) other: " + label


def gpu_list_breakdown(threads, lists, begin, end, to_ns, frames):
    """The command list copies' submissions on the GPU's clock: how long the GPU still ran earlier
    submissions after the copy's was submitted (queue), the submission's own work before the
    producer dispatch, the dispatch, the copy, and the end until the front saw it done.

    The GPU clock is mapped to the steady clock by the lower envelope of (GPU start - CPU
    vkQueueSubmit begin) per second: a submission can't start before it is submitted, and the
    first ones after an idle GPU start within microseconds of it. Its error is that latency."""
    gpu = collections.defaultdict(dict)  # tick -> event -> GPU ns
    submit_begin, submit_end = {}, {}
    for thread in threads.values():
        for t, c, a in zip(thread.ticks, thread.codes, thread.args):
            name = EVENTS[c & 0xFFFF] if c & 0xFFFF < len(EVENTS) else None
            if name in GPU_EVENTS:
                gpu[a][name] = t
            elif name == "WorkerSubmitBegin":
                submit_begin[a] = to_ns(t)
            elif name == "WorkerSubmitEnd":
                submit_end[a] = to_ns(t)
    if not gpu:
        return None
    window_ns = 1e9
    envelope = {}
    for tick, events in gpu.items():
        if "GpuSubmitStart" in events and tick in submit_begin:
            key = int(submit_begin[tick] // window_ns)
            delta = events["GpuSubmitStart"] - submit_begin[tick]
            envelope[key] = min(envelope.get(key, delta), delta)
    keys = sorted(envelope)
    if not keys:
        return None

    def offset(cpu_ns):
        """GPU ns - steady ns at cpu_ns, linear between the windows' envelopes."""
        x = cpu_ns / window_ns - 0.5
        i = bisect.bisect_right(keys, x) - 1
        if i < 0:
            return envelope[keys[0]]
        if i + 1 >= len(keys):
            return envelope[keys[-1]]
        k0, k1 = keys[i], keys[i + 1]
        w = (x - k0) / (k1 - k0)
        return envelope[k0] * (1 - w) + envelope[k1] * w

    ticks_sorted = sorted(gpu)
    ends = [gpu[t].get("GpuSubmitEnd") for t in ticks_sorted]
    # The latest end of every submission up to each one (the queue's backlog).
    backlog_end, latest = [], None
    for value in ends:
        if value is not None:
            latest = value if latest is None else max(latest, value)
        backlog_end.append(latest)
    index_of = {t: i for i, t in enumerate(ticks_sorted)}
    lo_ns, hi_ns = to_ns(begin), to_ns(end)
    busy = []
    for t in ticks_sorted:
        events = gpu[t]
        if "GpuSubmitStart" in events and "GpuSubmitEnd" in events and t in submit_begin and \
                lo_ns <= submit_begin[t] < hi_ns:
            busy.append((events["GpuSubmitStart"], events["GpuSubmitEnd"]))
    series = collections.defaultdict(list)
    totals = collections.Counter()
    depth = []
    violations = checked = 0
    for entry in lists:
        if not begin <= entry["suspend"] < end:
            continue
        tick = entry["tick"]
        events = gpu.get(tick)
        if not events or "GpuSubmitEnd" not in events or tick not in submit_end:
            series["(no GPU timestamps)"].append(0)
            continue
        off = offset(submit_end[tick])
        submitted = submit_end[tick] + off  # on the GPU clock
        i = index_of[tick]
        previous_end = backlog_end[i - 1] if i > 0 else None
        own_begin = max(submitted, previous_end) if previous_end is not None else submitted
        gpu_end = events["GpuSubmitEnd"]
        parts = {}
        parts["queue: earlier submissions still running"] = max(0.0, own_begin - submitted)
        producer, copy = events.get("GpuProducer"), events.get("GpuCopy")
        if producer is not None:
            parts["own submission: work before the producer"] = max(0.0, producer - own_begin)
            if copy is not None:
                parts["producer dispatch (to before the copy)"] = max(0.0, copy - producer)
        elif copy is not None:
            parts["own submission: work before the copy (no producer mark)"] = \
                max(0.0, copy - own_begin)
        if copy is not None:
            parts["copy (to the submission's end)"] = max(0.0, gpu_end - copy)
        else:
            parts["own submission (no marks)"] = max(0.0, gpu_end - own_begin)
        parts["submitted -> GPU end"] = gpu_end - submitted
        if entry["first_true"] is not None:
            seen = to_ns(entry["first_true"]) + off
            checked += 1
            if seen < gpu_end:
                violations += 1
            parts["GPU end -> done seen"] = seen - gpu_end
            parts["submitted -> done seen"] = seen - submitted
        parts["own submission GPU start (top of pipe) - submitted"] = \
            events.get("GpuSubmitStart", submitted) - submitted
        for name, value in parts.items():
            series[name].append(value / 1e3)
            totals[name] += value
        # Earlier submissions not done yet when this one was submitted.
        n = 0
        j = i - 1
        while j >= 0 and ends[j] is not None and ends[j] > submitted:
            n += 1
            j -= 1
        depth.append(n)
    result = {"windows": len(keys), "seen_before_end": violations, "checked": checked,
              "parts_us": {k: percentiles(v) for k, v in series.items()},
              "parts_ms_per_frame": {k: v / 1e6 / max(frames, 1) for k, v in totals.items()},
              "queue_depth": percentiles(depth).replace(" us", "")}
    if busy:
        busy.sort()
        covered, cursor = 0, None
        for a, b in busy:
            if cursor is None or a > cursor:
                covered += b - a
                cursor = b
            elif b > cursor:
                covered += b - cursor
                cursor = b
        span = busy[-1][1] - busy[0][0]
        result["gpu_busy"] = covered / span if span > 0 else 0.0
        result["submissions_per_frame"] = len(busy) / max(frames, 1)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("trace")
    parser.add_argument("--marks")
    parser.add_argument("--presents")
    parser.add_argument("--last-seconds", type=float, default=40.0)
    parser.add_argument("--json")
    options = parser.parse_args()

    threads, calibrations = read(options.trace)
    to_ns, scale = clock(calibrations)
    us = lambda ticks: ticks * scale / 1e3
    back = role(threads, "BackIdle")
    front = role(threads, "PushBegin")
    if back is None or front is None:
        sys.exit("no split GPU threads in the trace")
    presents = sorted(t for thread in threads.values()
                      for t, c in zip(thread.ticks, thread.codes) if c & 0xFFFF == E["Present"])
    traced_end = min(back.ticks[-1], front.ticks[-1])
    first_present = last_present = None
    if options.marks:
        with open(options.marks) as handle:
            marks = json.load(handle)
        first_present, last_present = marks["measure"], marks["end"]
    elif options.presents:
        first_present, last_present = (int(v) for v in options.presents.split(":"))
    if first_present is not None:
        if last_present > len(presents):
            print(f"note: the trace has {len(presents)} presents, the window ends at "
                  f"{last_present}: clipped")
            last_present = len(presents)
        begin, end = presents[first_present - 1], presents[last_present - 1]
    else:
        end = traced_end
        begin = end - int(options.last_seconds * 1e9 / scale)
    end = min(end, traced_end)
    frames = sum(1 for p in presents if begin < p <= end)
    wall_ms = us(end - begin) / 1e3
    print("threads: " + ", ".join(f"{t.index}:{t.name or '?'} ({len(t.ticks)} events)"
                                   for t in threads.values()))
    print(f"window: {wall_ms:.0f} ms, {frames} presents ({wall_ms / max(frames, 1):.2f} "
          f"ms/frame); trace has {len(presents)} presents")
    per_frame = lambda ticks: us(ticks) / 1e3 / max(frames, 1)

    idle, sleep, hooks = back_intervals(back, begin, end)
    acquires, lists = suspensions(front)
    classes = {(a["channel"], a["suspend"]): acquire_class(a) for a in acquires}
    segments = front_segments(front, classes)
    front_moves = transitions(front, classes)

    result = {"window_ms": wall_ms, "frames": frames}
    print(f"\nback idle: {per_frame(sum(b - a for a, b, *_ in idle)):.2f} ms/frame "
          f"({len(idle) / max(frames, 1):.0f}/frame); asleep (futex) "
          f"{per_frame(sum(b - a for a, b in sleep)):.2f} ms/frame "
          f"({len(sleep) / max(frames, 1):.0f}/frame); idle hook "
          f"{per_frame(sum(b - a for a, b, _ in hooks)):.2f} ms/frame")
    for name, spans in (("asleep", sleep), ("idle", idle)):
        totals = attribute(spans, segments)
        # The front's state when a span began names the span for the counts.
        counts = collections.Counter()
        starts = [s[0] for s in segments]
        for begin_span, *_ in spans:
            i = bisect.bisect_right(starts, begin_span) - 1
            counts[cause(segments[i][2]) if i >= 0 else "untraced"] += 1
        by_cause = collections.Counter()
        for label, ticks in totals.items():
            by_cause[cause(label) if label != "untraced" else "untraced"] += ticks
        print(f"\nback {name} by cause (ms/frame, spans starting in it per frame):")
        for label, ticks in by_cause.most_common():
            print(f"  {label:52} {per_frame(ticks):6.3f}  {counts[label] / max(frames, 1):6.1f}")
        print(f"back {name} by front state (ms/frame):")
        for label, ticks in totals.most_common(24):
            print(f"  {label:52} {per_frame(ticks):6.3f}")
        result[name] = {"by_cause_ms_per_frame": {k: per_frame(v) for k, v in by_cause.items()},
                        "by_state_ms_per_frame": {k: per_frame(v) for k, v in totals.items()},
                        "spans_per_frame_by_cause": {k: v / max(frames, 1)
                                                     for k, v in counts.items()}}

    ended_idle, ended_sleep, ended_counts = ended_by(idle, sleep, front_moves, segments)
    print("\nback idle spans by the front wait that ended last before the back got work "
          "(idle ms/frame, asleep ms/frame, spans/frame):")
    for label, ticks in ended_idle.most_common(16):
        print(f"  {label:44} {per_frame(ticks):6.3f} {per_frame(ended_sleep[label]):6.3f} "
              f"{ended_counts[label] / max(frames, 1):6.1f}")
    result["ended_by"] = {k: {"idle_ms_per_frame": per_frame(v),
                              "asleep_ms_per_frame": per_frame(ended_sleep[k]),
                              "spans_per_frame": ended_counts[k] / max(frames, 1)}
                          for k, v in ended_idle.items()}

    # Command list copies: back flush (dispatch recorded) -> worker submit -> GPU done -> resume.
    flushes, submits = {}, {}
    for t, c, a in zip(back.ticks, back.codes, back.args):
        if c & 0xFFFF == E["ListCopyFlush"]:
            flushes[a] = t
    for thread in threads.values():
        for t, c, a in zip(thread.ticks, thread.codes, thread.args):
            if c & 0xFFFF == E["WorkerSubmitEnd"]:
                submits[a] = t
    back_runs = array.array("Q", (t for t, c in zip(back.ticks, back.codes)
                                  if c & 0xFFFF == E["BackRun"]))
    next_run = lambda t: back_runs[i] if (i := bisect.bisect_right(back_runs, t)) < len(back_runs) else None
    series = collections.defaultdict(list)
    for entry in lists:
        if not begin <= entry["suspend"] < end:
            continue
        flush, submit = flushes.get(entry["tick"]), submits.get(entry["tick"])
        if flush is not None and flush <= entry["suspend"]:
            series["dispatch(flush) -> front suspended"].append(us(entry["suspend"] - flush))
            if submit is not None:
                series["dispatch(flush) -> worker submitted"].append(us(submit - flush))
        if submit is not None and entry["first_true"] is not None:
            series["submitted -> GPU done seen (first true poll)"].append(
                us(entry["first_true"] - submit))
            if entry["last_false"] is not None and entry["last_false"] > submit:
                series["submitted -> last poll not done (lower bound)"].append(
                    us(entry["last_false"] - submit))
        if entry["first_true"] is not None:
            series["GPU done seen -> front resumed"].append(us(entry["resume"] - entry["first_true"]))
        series["suspended -> resumed"].append(us(entry["resume"] - entry["suspend"]))
        if (run := next_run(entry["resume"])) is not None:
            series["resumed -> back runs again"].append(us(run - entry["resume"]))
    print(f"\ncommand list copy waits: {sum(1 for e in lists if begin <= e['suspend'] < end) / max(frames, 1):.1f}/frame")
    for name, values in series.items():
        print(f"  {name:48} {percentiles(values)}")
    result["list_copy_us"] = {k: percentiles(v) for k, v in series.items()}

    breakdown = gpu_list_breakdown(threads, lists, begin, end, to_ns, frames)
    if breakdown is not None:
        print(f"\ncommand list copies on the GPU clock ({breakdown['windows']} 1 s windows; "
              f"done seen before the GPU end {breakdown['seen_before_end']}/"
              f"{breakdown['checked']}; GPU busy {breakdown.get('gpu_busy', 0) * 100:.0f}% "
              f"(submission start to end), {breakdown.get('submissions_per_frame', 0):.0f} "
              f"submissions/frame):")
        for name, text in breakdown["parts_us"].items():
            print(f"  {name:58} {breakdown['parts_ms_per_frame'].get(name, 0):6.3f} ms/frame  {text}")
        print(f"  earlier submissions not done when submitted: {breakdown['queue_depth']}")
        result["gpu_list_copy"] = breakdown

    # Acquires the channels waited on.
    print("\nacquire waits by class (per frame):")
    by_class = collections.defaultdict(list)
    for entry in acquires:
        if begin <= entry["suspend"] < end:
            by_class[acquire_class(entry)].append(entry)
    result["acquire_us"] = {}
    for name, entries in sorted(by_class.items()):
        addresses = collections.Counter(f"{e['address']:#x}" for e in entries)
        print(f"  {name}: {len(entries) / max(frames, 1):.1f}/frame, words "
              + ", ".join(f"{a} x{n}" for a, n in addresses.most_common(6)))
        series = collections.defaultdict(list)
        for e in entries:
            if e["drain"] is not None and e["drain_end"] is not None and e["drain_end"] >= e["drain"]:
                series["acquire drain (back reaches the acquire)"].append(us(e["drain_end"] - e["drain"]))
            if e["drain_end"] is not None and e["suspend"] >= e["drain_end"]:
                series["back reached -> suspended (fences)"].append(us(e["suspend"] - e["drain_end"]))
            if e["seen"] is not None:
                series["suspended -> release seen"].append(us(e["seen"] - e["suspend"]))
                if e["last_waiting"] is not None:
                    series["last unmet poll -> release seen"].append(us(e["seen"] - e["last_waiting"]))
                series["release seen -> front resumed"].append(us(e["resume"] - e["seen"]))
            if (run := next_run(e["resume"])) is not None:
                series["resumed -> back runs again"].append(us(run - e["resume"]))
            series["polls"].append(e["polls"])
        for key, values in series.items():
            text = percentiles(values)
            if key == "polls":
                text = text.replace(" us", "")
            print(f"    {key:46} {text}")
        result["acquire_us"][name] = {k: percentiles(v) for k, v in series.items()}

    counts = collections.Counter()
    for thread in (front, back):
        lo = bisect.bisect_left(thread.ticks, begin)
        hi = bisect.bisect_left(thread.ticks, end)
        for c in thread.codes[lo:hi]:
            counts[EVENTS[c & 0xFFFF] if c & 0xFFFF < len(EVENTS) else str(c & 0xFFFF)] += 1
    print("\nfront and back events per frame: " + ", ".join(
        f"{k} {v / max(frames, 1):.0f}" for k, v in counts.most_common()))
    result["events_per_frame"] = {k: v / max(frames, 1) for k, v in counts.items()}
    if options.json:
        with open(options.json, "w") as handle:
            json.dump(result, handle, indent=1)


if __name__ == "__main__":
    main()
