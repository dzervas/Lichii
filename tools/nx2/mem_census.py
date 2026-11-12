#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What keeping guest memory coherent between the CPU and the GPU costs, per guest range, from an
EDEN_NX2_MEM_CENSUS file.

  mem_census.py <census> [--frames FIRST:LAST | --marks marks.json] [--top N] [--exact]
                [--stream-share S] [--max-group-mib M] [--json OUT]

The census sums each thread's events per frame (a frame ends at a present) and per (event,
detail, range) (src/common/mem_census.h). Frames are counted from the first present after the
census started; --frames takes FIRST..LAST inclusive (FIRST: or :LAST leave one end open),
--marks bench.py's marks.json "measure".."end" presents (the emulator's present count, which
the census's frame numbers follow from boot).

Ranges are grouped: the ranges the buffer and texture caches name (bind ranges of uploads and GPU
writes, images, download pieces, waits) are merged where they overlap, and the page-sized events
(CPU write traps, protection changes) go to the group holding their page; pages no group holds
form groups of their own, adjacent pages together. Ranges over --max-group-mib (default 64;
sparse images name gigabytes) are groups of their own that take nothing in. --exact keeps every range the census named
apart instead (page events then stay per page or protection run).

Printed: the top groups by upload bytes, protection changes, CPU write traps and read waits; a
class per group over the frames (see classify); the totals per frame of each class; and the
events and timed cost per thread. Upload "unchanged" bytes are those whose hash matched the
range's previous upload (texture uploads are not hashed and count as changed).

Standard library only.
"""

import argparse
import bisect
import collections
import json
import struct
import sys

FILE_MAGIC = b"MEMCENS1"
BLOCK_MAGIC = 0x4E45434D
BLOCK = struct.Struct("<IHHII")
RECORD = struct.Struct("<QQQQIIBB6x")
THREAD_NAME, RECORDS, PRESENT = 0, 1, 2

# src/common/mem_census.h
EVENTS = ["cpu-write", "protect", "unprotect", "upload", "gpu-write", "download", "read-wait"]
CPU_WRITE, PROTECT, UNPROTECT, UPLOAD, GPU_WRITE, DOWNLOAD, READ_WAIT = range(len(EVENTS))
CPU_WRITES = ["trap", "invalidate", "inline-engine"]
TRAP, INVALIDATE, INLINE_ENGINE = range(len(CPU_WRITES))
PROTECTIONS = ["direct", "deferred"]
BINDS = ["none", "vertex", "index", "indirect", "uniform", "uniform-stream", "uniform-compute",
         "uniform-stream-compute", "storage", "storage-compute", "texture-buffer",
         "texture-buffer-compute", "transform-feedback", "dma", "dma-clear", "inline", "obtain",
         "native-bank", "texture", "image"]
TEXTURE_BIND = BINDS.index("texture")
REASONS = ["other", "guest-read", "early-read", "command-list", "query", "macro-read", "flush",
           "texture-gc", "cpu-write-over-gpu"]
PAGE = 4096
MAX_GROUP = 64 << 20

CLASSES = ["cpu-streamed", "cpu-static", "gpu-written-cpu-read", "gpu-written-gpu-read", "mixed"]


def name_of(table, index):
    return table[index] if index < len(table) else f"#{index}"


def detail_name(event, detail):
    if event == CPU_WRITE:
        return name_of(CPU_WRITES, detail)
    if event in (PROTECT, UNPROTECT):
        return name_of(PROTECTIONS, detail)
    if event in (UPLOAD, GPU_WRITE):
        return name_of(BINDS, detail)
    return name_of(REASONS, detail)


class Entry:
    """One (thread, event, detail, range)'s sums over the frames read, and the frames (bit sets)
    it was seen in and, for uploads, had changed bytes in."""

    __slots__ = ("count", "bytes", "ns", "aux", "frames", "changed")

    def __init__(self):
        self.count = self.bytes = self.ns = self.aux = 0
        self.frames = self.changed = 0


class Census:
    """A census file's records within a span of frames, summed per (thread, event, detail, addr,
    size): a run has millions of records over a few thousand ranges."""

    def __init__(self):
        self.threads = {}  # index -> name
        self.presents = {}  # frame -> steady clock ns
        self.entries = collections.defaultdict(Entry)  # (thread, event, detail, addr, size)
        self.records = 0
        self.frames = set()


def read_census(path, first=None, last=None):
    census = Census()
    with open(path, "rb") as handle:
        header = handle.read(16)
        if header[:8] != FILE_MAGIC:
            raise ValueError(f"{path}: not a memory census file")
        record_size = struct.unpack_from("<I", header, 8)[0]
        if record_size != RECORD.size:
            raise ValueError(f"{path}: records of {record_size} bytes, expected {RECORD.size}")
        while True:
            block = handle.read(BLOCK.size)
            if len(block) < BLOCK.size:
                break
            magic, kind, thread, frame, count = BLOCK.unpack(block)
            if magic != BLOCK_MAGIC:
                raise ValueError(f"{path}: bad block at {handle.tell() - BLOCK.size}")
            if kind == THREAD_NAME:
                name = handle.read(16).split(b"\0", 1)[0].decode(errors="replace")
                census.threads[thread] = name or f"thread{thread}"
            elif kind == PRESENT:
                payload = handle.read(8)
                if len(payload) < 8:
                    break
                census.presents[frame] = struct.unpack("<Q", payload)[0]
            elif kind == RECORDS:
                if (first is not None and frame < first) or (last is not None and frame > last):
                    handle.seek(count * RECORD.size, 1)
                    continue
                payload = handle.read(count * RECORD.size)
                if len(payload) < count * RECORD.size:
                    break  # a block the writer had not finished
                census.frames.add(frame)
                census.records += count
                bit = 1 << frame
                entries = census.entries
                for addr, size_bytes, ns, aux, size, n, event, detail in RECORD.iter_unpack(
                        payload):
                    entry = entries[(thread, event, detail, addr, size)]
                    entry.count += n
                    entry.bytes += size_bytes
                    entry.ns += ns
                    entry.aux += aux
                    entry.frames |= bit
                    if aux < size_bytes:
                        entry.changed |= bit
            else:
                raise ValueError(f"{path}: unknown block type {kind}")
    return census


def frame_span(census, first, last):
    """The frames the totals are divided by: the presents in the span."""
    presents = [frame for frame in census.presents
                if (first is None or frame >= first) and (last is None or frame <= last)]
    if presents:
        return len(presents)
    return max(1, len(census.frames))


def is_page_event(event):
    return event in (CPU_WRITE, PROTECT, UNPROTECT)


class Groups:
    """Ranges merged where they overlap; finds the group holding an address."""

    def __init__(self, intervals):
        merged = []
        for begin, end in sorted(intervals):
            if merged and begin < merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([begin, end])
        self.starts = [begin for begin, _ in merged]
        self.ranges = merged

    def find(self, addr):
        index = bisect.bisect_right(self.starts, addr) - 1
        if index >= 0 and addr < self.ranges[index][1]:
            return index
        return None


def group_records(census, exact=False, max_group=MAX_GROUP):
    """The entries of each group, {(begin, end): [(key, entry, share)]}. A page event over several
    pages is split per page, `share` its fraction on each. Ranges over `max_group` bytes (sparse
    images) stay groups of their own and take in nothing."""
    out = collections.defaultdict(list)
    if exact:
        for key, entry in census.entries.items():
            out[(key[3], key[3] + max(key[4], 1))].append((key, entry, 1.0))
        return out
    groups = Groups((key[3], key[3] + max(key[4], 1)) for key in census.entries
                    if not is_page_event(key[1]) and key[4] <= max_group)
    orphan_pages = set()
    page_hits = []
    for key, entry in census.entries.items():
        thread, event, detail, addr, size = key
        if not is_page_event(event):
            if size > max_group:
                out[(addr, addr + size)].append((key, entry, 1.0))
                continue
            begin, end = groups.ranges[groups.find(addr)]
            out[(begin, end)].append((key, entry, 1.0))
            continue
        first_page = addr // PAGE
        pages = (addr + max(size, 1) - 1) // PAGE - first_page + 1
        for page in range(first_page, first_page + pages):
            index = groups.find(page * PAGE)
            if index is None:
                index = groups.find(page * PAGE + PAGE - 1)
            if index is None:
                orphan_pages.add(page)
            page_hits.append((index, page, key, entry, 1.0 / pages))
    # Runs of adjacent pages no group holds.
    run_of = {}
    run_end = {}
    previous = None
    for page in sorted(orphan_pages):
        begin = run_of[previous] if previous is not None and page == previous + 1 else page
        run_of[page] = begin
        run_end[begin] = page + 1
        previous = page
    for index, page, key, entry, share in page_hits:
        if index is None:
            begin = run_of[page]
            group = (begin * PAGE, run_end[begin] * PAGE)
        else:
            group = tuple(groups.ranges[index])
        out[group].append((key, entry, share))
    return out


class GroupStats:
    def __init__(self, begin, end):
        self.begin = begin
        self.end = end
        self.klass = None
        self.sums = collections.Counter()
        self.binds = collections.Counter()  # upload and GPU write bytes by bind
        self.reasons = collections.Counter()  # download bytes and wait ns by reason
        self.frames = collections.Counter()  # bit sets: what happened in which frames

    def add(self, key, entry, share):
        thread, event, detail, addr, size = key
        count, size_bytes, ns, aux = entry.count, entry.bytes, entry.ns, entry.aux
        frame = entry.frames
        sums = self.sums
        frames = self.frames
        if event == CPU_WRITE:
            if detail == TRAP:
                sums["traps"] += count * share
                sums["trap_bytes"] += size_bytes * share
            elif detail == INVALIDATE:
                sums["invalidates"] += count * share
                sums["invalidate_ns"] += ns * share
                sums["collected"] += aux * share
            else:
                sums["inline_engine"] += count * share
                sums["inline_engine_bytes"] += size_bytes * share
            frames["cpu_write"] |= frame
        elif event in (PROTECT, UNPROTECT):
            kind = "protect" if event == PROTECT else "unprotect"
            sums[kind + "_calls"] += count * share
            sums[kind + "_pages"] += size_bytes / PAGE * share
            sums[kind + "_ns"] += ns * share
            if event == UNPROTECT and detail == 1:
                sums["deferred_unprotect_pages"] += size_bytes / PAGE * share
            frames[kind] |= frame
        elif event == UPLOAD:
            sums["uploads"] += count
            sums["upload_bytes"] += size_bytes
            unchanged = 0 if detail == TEXTURE_BIND else aux
            sums["unchanged_bytes"] += unchanged
            self.binds["upload " + name_of(BINDS, detail)] += size_bytes
            frames["upload"] |= frame
            frames["changed_upload"] |= frame if detail == TEXTURE_BIND else entry.changed
        elif event == GPU_WRITE:
            sums["gpu_writes"] += count
            sums["gpu_write_bytes"] += size_bytes
            self.binds["gpu " + name_of(BINDS, detail)] += size_bytes
            frames["gpu_write"] |= frame
        elif event == DOWNLOAD:
            sums["downloads"] += count
            sums["download_bytes"] += size_bytes
            self.reasons["download " + name_of(REASONS, detail)] += size_bytes
            frames["cpu_read"] |= frame
        elif event == READ_WAIT:
            sums["waits"] += count
            sums["wait_ns"] += ns
            self.reasons["wait " + name_of(REASONS, detail)] += ns
            frames["cpu_read"] |= frame

    def label(self):
        top = [name for name, _ in self.binds.most_common(2)]
        if not top:
            top = [name for name, _ in self.reasons.most_common(1)]
        if not top and self.sums["traps"]:
            top = ["cpu writes"]
        return ", ".join(top) or "-"


def classify(stats, stream_share=0.5):
    """The group's class over the frames read.

    gpu-written-cpu-read: the GPU writes it and the CPU reads it back (downloads or read waits),
    the CPU never writes it. gpu-written-gpu-read: the GPU writes it, the CPU neither reads nor
    writes it. mixed: both the CPU and the GPU write it. cpu-streamed: only the CPU writes it, and
    it is rewritten (CPU write traps or uploads of changed bytes) in at least `stream_share` of
    the frames it is uploaded or written in. cpu-static: the rest (uploaded again with the same
    bytes, or rarely rewritten).
    """
    frames = stats.frames
    gpu_written = bool(frames["gpu_write"])
    cpu_written = bool(frames["cpu_write"] | frames["changed_upload"])
    if gpu_written:
        if cpu_written:
            return "mixed"
        return "gpu-written-cpu-read" if frames["cpu_read"] else "gpu-written-gpu-read"
    active = frames["upload"] | frames["cpu_write"]
    rewritten = frames["changed_upload"] | frames["cpu_write"]
    if active and rewritten.bit_count() >= stream_share * active.bit_count():
        return "cpu-streamed"
    return "cpu-static"


def analyze(census, exact=False, stream_share=0.5, max_group=MAX_GROUP):
    groups = {}
    for (begin, end), entries in group_records(census, exact, max_group).items():
        stats = GroupStats(begin, end)
        for key, entry, share in entries:
            stats.add(key, entry, share)
        stats.klass = classify(stats, stream_share)
        groups[(begin, end)] = stats
    return groups


def thread_costs(census):
    """Event counts and ns per (thread name, event, detail) over the frames read."""
    costs = collections.Counter()
    counts = collections.Counter()
    for (thread, event, detail, addr, size), entry in census.entries.items():
        key = (census.threads.get(thread, f"thread{thread}"), name_of(EVENTS, event),
               detail_name(event, detail))
        costs[key] += entry.ns
        counts[key] += entry.count
    return costs, counts


def mib(value):
    return value / (1 << 20)


def print_report(census, groups, frames, top, out=None):
    out = out or sys.stdout

    def p(text=""):
        print(text, file=out)

    def per_frame_ms(ns):
        return ns / frames / 1e6

    p(f"{frames} frames, {census.records} records, {len(census.entries)} ranges per thread and "
      f"kind, {len(groups)} groups")
    ranked = [
        ("upload bytes", lambda s: s.sums["upload_bytes"],
         lambda s: f"{mib(s.sums['upload_bytes']) / frames:8.3f} MiB/f "
                   f"{s.sums['uploads'] / frames:8.1f} uploads/f unchanged "
                   f"{100 * s.sums['unchanged_bytes'] / max(1, s.sums['upload_bytes']):5.1f}%"),
        ("protection changes", lambda s: s.sums["protect_pages"] + s.sums["unprotect_pages"],
         lambda s: f"protect {s.sums['protect_pages'] / frames:8.1f} pg/f "
                   f"{s.sums['protect_calls'] / frames:6.1f} calls/f, unprotect "
                   f"{s.sums['unprotect_pages'] / frames:8.1f} pg/f (deferred "
                   f"{s.sums['deferred_unprotect_pages'] / frames:.1f}), "
                   f"{per_frame_ms(s.sums['protect_ns'] + s.sums['unprotect_ns']):6.3f} ms/f"),
        ("CPU write traps", lambda s: s.sums["traps"],
         lambda s: f"{s.sums['traps'] / frames:9.1f} traps/f "
                   f"{s.sums['invalidates'] / frames:7.1f} OnCPUWrite/f "
                   f"{per_frame_ms(s.sums['invalidate_ns']):6.3f} ms/f, collected "
                   f"{s.sums['collected'] / frames:.1f}/f"),
        ("read waits", lambda s: s.sums["wait_ns"],
         lambda s: f"{per_frame_ms(s.sums['wait_ns']):7.3f} ms/f "
                   f"{s.sums['waits'] / frames:6.2f} waits/f, downloaded "
                   f"{s.sums['download_bytes'] / frames / 1024:8.1f} KiB/f "
                   + " ".join(f"{name[5:]}={per_frame_ms(value):.3f}ms"
                              for name, value in s.reasons.most_common()
                              if name.startswith("wait "))),
    ]
    for title, key, describe in ranked:
        chosen = sorted((s for s in groups.values() if key(s) > 0), key=key, reverse=True)[:top]
        p()
        p(f"Top groups by {title} (per frame)")
        for stats in chosen:
            span = f"{stats.begin:#x}+{stats.end - stats.begin:#x}"
            p(f"  {span:>26} {stats.klass:<20} {describe(stats)}  [{stats.label()}]")

    p()
    p("Classes, totals per frame")
    p(f"  {'class':<20} {'groups':>6} {'upload MiB':>10} {'unchg%':>6} {'traps':>8} "
      f"{'OnCPUWr ms':>10} {'prot pg':>8} {'unprot pg':>9} {'prot ms':>8} {'gpu wr':>7} "
      f"{'dl KiB':>8} {'wait ms':>8}")
    for klass in CLASSES:
        members = [s for s in groups.values() if s.klass == klass]
        total = collections.Counter()
        for stats in members:
            total.update(stats.sums)
        p(f"  {klass:<20} {len(members):>6} {mib(total['upload_bytes']) / frames:>10.3f} "
          f"{100 * total['unchanged_bytes'] / max(1, total['upload_bytes']):>6.1f} "
          f"{total['traps'] / frames:>8.1f} {per_frame_ms(total['invalidate_ns']):>10.3f} "
          f"{total['protect_pages'] / frames:>8.1f} {total['unprotect_pages'] / frames:>9.1f} "
          f"{per_frame_ms(total['protect_ns'] + total['unprotect_ns']):>8.3f} "
          f"{total['gpu_writes'] / frames:>7.1f} {total['download_bytes'] / frames / 1024:>8.1f} "
          f"{per_frame_ms(total['wait_ns']):>8.3f}")

    costs, counts = thread_costs(census)
    p()
    p("Per thread, per frame: events and their timed cost")
    for key, count in sorted(counts.items(), key=lambda item: (-costs[item[0]], -item[1])):
        thread, event, detail = key
        p(f"  {thread:<16} {event:<10} {detail:<22} {count / frames:10.1f}/f "
          f"{per_frame_ms(costs[key]):8.3f} ms/f")


def to_json(groups, frames):
    return {
        "frames": frames,
        "groups": [{
            "begin": stats.begin, "end": stats.end, "class": stats.klass, "label": stats.label(),
            "per_frame": {key: value / frames for key, value in stats.sums.items()},
            "binds": dict(stats.binds), "reasons": dict(stats.reasons),
            "frames": {key: value.bit_count() for key, value in stats.frames.items()},
        } for stats in sorted(groups.values(), key=lambda s: s.begin)],
    }


def parse_frames(text):
    first, _, last = text.partition(":")
    return (int(first) if first else None), (int(last) if last else None)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("census")
    parser.add_argument("--frames", help="FIRST:LAST, inclusive; either end may be left out")
    parser.add_argument("--marks", help="bench.py's marks.json: its measure..end presents")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--exact", action="store_true", help="keep every range apart")
    parser.add_argument("--stream-share", type=float, default=0.5)
    parser.add_argument("--max-group-mib", type=float, default=MAX_GROUP / (1 << 20),
                        help="ranges larger than this are not merged with others")
    parser.add_argument("--json")
    options = parser.parse_args(argv)
    first = last = None
    if options.marks:
        with open(options.marks) as handle:
            marks = json.load(handle)
        first, last = marks["measure"], marks["end"]
    elif options.frames:
        first, last = parse_frames(options.frames)
    census = read_census(options.census, first, last)
    frames = frame_span(census, first, last)
    groups = analyze(census, options.exact, options.stream_share,
                     int(options.max_group_mib * (1 << 20)))
    print_report(census, groups, frames, options.top)
    if options.json:
        with open(options.json, "w") as handle:
            json.dump(to_json(groups, frames), handle, indent=1)


if __name__ == "__main__":
    main()
