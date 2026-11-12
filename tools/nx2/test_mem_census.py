#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mem_census as mc  # noqa: E402

BIND = {name: index for index, name in enumerate(mc.BINDS)}
REASON = {name: index for index, name in enumerate(mc.REASONS)}


def record(event, detail, addr, size, count=1, size_bytes=None, ns=0, aux=0):
    return mc.RECORD.pack(addr, size if size_bytes is None else size_bytes, ns, aux, size, count,
                          event, detail)


class Writer:
    """Builds a census file as src/common/mem_census.cpp writes it."""

    def __init__(self):
        self.data = bytearray(b"MEMCENS1" + struct.pack("<II", mc.RECORD.size, 0))

    def thread(self, index, name):
        self.data += mc.BLOCK.pack(mc.BLOCK_MAGIC, mc.THREAD_NAME, index, 0, 0)
        self.data += name.encode().ljust(16, b"\0")

    def records(self, thread, frame, records):
        self.data += mc.BLOCK.pack(mc.BLOCK_MAGIC, mc.RECORDS, thread, frame, len(records))
        for entry in records:
            self.data += entry

    def present(self, frame, ns):
        self.data += mc.BLOCK.pack(mc.BLOCK_MAGIC, mc.PRESENT, 0, frame, 0)
        self.data += struct.pack("<Q", ns)


# Ranges of the synthetic run, one per class.
STREAMED = 0x100000  # vertex data the CPU rewrites every frame
STATIC = 0x200000  # uniforms uploaded again with the same bytes
READ_BACK = 0x300000  # storage a compute pass writes, read back by the CPU
GPU_ONLY = 0x400000  # a render target
MIXED = 0x500000  # storage the GPU writes and the CPU writes into
ORPHAN = 0x900000  # pages the CPU writes that no cache range names


def frame_records(frame):
    back = [
        record(mc.UPLOAD, BIND["vertex"], STREAMED, 0x2000, aux=0),
        record(mc.UPLOAD, BIND["uniform-stream"], STATIC, 0x100, count=10,
               size_bytes=0xA00, aux=0xA00 if frame else 0),
        # Another uniform inside the same range: grouped with it.
        record(mc.UPLOAD, BIND["uniform-stream"], STATIC + 0x80, 0x100, aux=0x100),
        record(mc.GPU_WRITE, BIND["storage-compute"], READ_BACK, 0x1000),
        record(mc.GPU_WRITE, BIND["image"], GPU_ONLY, 0x10000),
        record(mc.GPU_WRITE, BIND["storage"], MIXED, 0x1000),
        record(mc.PROTECT, 0, STREAMED, 0x2000, ns=3000),
    ]
    core = [
        record(mc.CPU_WRITE, mc.TRAP, STREAMED, 0x1000, count=100, size_bytes=800),
        record(mc.CPU_WRITE, mc.TRAP, STREAMED + 0x1000, 0x1000, count=50, size_bytes=400),
        record(mc.CPU_WRITE, mc.INVALIDATE, STREAMED, 0x1000, ns=20000),
        record(mc.UNPROTECT, 1, STREAMED, 0x2000, ns=4000),
        record(mc.CPU_WRITE, mc.TRAP, MIXED, 0x1000, count=4, size_bytes=16),
        record(mc.CPU_WRITE, mc.TRAP, ORPHAN, 0x1000, count=2, size_bytes=8),
        record(mc.CPU_WRITE, mc.TRAP, ORPHAN + 0x1000, 0x1000, count=2, size_bytes=8),
        record(mc.CPU_WRITE, mc.INLINE_ENGINE, MIXED, 0x1000, count=3, size_bytes=12),
        record(mc.READ_WAIT, REASON["guest-read"], READ_BACK, 0x1000, ns=500000),
        record(mc.DOWNLOAD, REASON["guest-read"], READ_BACK, 0x1000),
    ]
    return back, core


def census_file(directory, frames=4):
    writer = Writer()
    writer.thread(0, "GPUBack")
    writer.thread(1, "CPUCore_0")
    for frame in range(frames):
        back, core = frame_records(frame)
        writer.records(0, frame, back)
        writer.records(1, frame, core)
        writer.present(frame, 1000 + frame * 16_000_000)
    path = os.path.join(directory, "census.bin")
    with open(path, "wb") as handle:
        handle.write(writer.data)
    return path


class MemCensusTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = census_file(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def groups(self, **options):
        census = mc.read_census(self.path)
        return census, mc.analyze(census, **options)

    def test_reads_threads_presents_and_records(self):
        census = mc.read_census(self.path)
        self.assertEqual(census.threads, {0: "GPUBack", 1: "CPUCore_0"})
        self.assertEqual(sorted(census.presents), [0, 1, 2, 3])
        self.assertEqual(census.records, 4 * 17)
        # Summed per (thread, event, detail, range), with the frames each was seen in.
        self.assertEqual(len(census.entries), 17)
        entry = census.entries[(1, mc.CPU_WRITE, mc.TRAP, STREAMED, 0x1000)]
        self.assertEqual((entry.count, entry.bytes, entry.frames), (400, 3200, 0b1111))
        static = census.entries[(0, mc.UPLOAD, BIND["uniform-stream"], STATIC, 0x100)]
        self.assertEqual(static.changed, 0b0001)
        self.assertEqual(mc.frame_span(census, None, None), 4)

    def test_frame_span_selects_records(self):
        census = mc.read_census(self.path, 1, 2)
        self.assertEqual(census.frames, {1, 2})
        self.assertEqual(census.records, 2 * 17)
        self.assertEqual(mc.frame_span(census, 1, 2), 2)
        self.assertEqual(mc.parse_frames("5:"), (5, None))
        self.assertEqual(mc.parse_frames(":7"), (None, 7))

    def test_groups_overlapping_ranges_and_their_pages(self):
        _, groups = self.groups()
        streamed = groups[(STREAMED, STREAMED + 0x2000)]
        self.assertEqual(streamed.sums["traps"], 4 * 150)
        self.assertEqual(streamed.sums["protect_pages"], 4 * 2)
        self.assertEqual(streamed.sums["deferred_unprotect_pages"], 4 * 2)
        self.assertEqual(streamed.sums["invalidate_ns"], 4 * 20000)
        static = groups[(STATIC, STATIC + 0x180)]
        self.assertEqual(static.sums["uploads"], 4 * 11)
        # Orphan pages: one group for the two adjacent ones.
        self.assertIn((ORPHAN, ORPHAN + 0x2000), groups)
        # Inline engine writes are page events too.
        self.assertEqual(groups[(MIXED, MIXED + 0x1000)].sums["inline_engine"], 4 * 3)

    def test_large_ranges_take_nothing_in(self):
        census = mc.read_census(self.path)
        huge = mc.Entry()
        huge.count, huge.bytes, huge.frames = 1, 1 << 30, 0b1
        census.entries[(0, mc.UPLOAD, BIND["texture"], 0, 1 << 30)] = huge
        groups = mc.analyze(census)
        self.assertIn((0, 1 << 30), groups)
        self.assertIn((STREAMED, STREAMED + 0x2000), groups)
        self.assertEqual(groups[(0, 1 << 30)].sums["traps"], 0)

    def test_exact_keeps_ranges_apart(self):
        _, groups = self.groups(exact=True)
        self.assertIn((STATIC + 0x80, STATIC + 0x180), groups)
        self.assertIn((ORPHAN + 0x1000, ORPHAN + 0x2000), groups)

    def test_classifies_each_range(self):
        _, groups = self.groups()
        classes = {begin: stats.klass for (begin, _), stats in groups.items()}
        self.assertEqual(classes[STREAMED], "cpu-streamed")
        self.assertEqual(classes[STATIC], "cpu-static")
        self.assertEqual(classes[READ_BACK], "gpu-written-cpu-read")
        self.assertEqual(classes[GPU_ONLY], "gpu-written-gpu-read")
        self.assertEqual(classes[MIXED], "mixed")
        self.assertEqual(classes[ORPHAN], "cpu-streamed")

    def test_stream_share_threshold(self):
        stats = mc.GroupStats(0, 0x1000)
        entry = mc.Entry()
        entry.count, entry.bytes, entry.aux = 4, 0x400, 0x300
        entry.frames, entry.changed = 0b1111, 0b0001
        stats.add((0, mc.UPLOAD, BIND["uniform"], 0, 0x100), entry, 1.0)
        self.assertEqual(mc.classify(stats, 0.5), "cpu-static")
        self.assertEqual(mc.classify(stats, 0.25), "cpu-streamed")

    def test_texture_uploads_count_as_changed(self):
        stats = mc.GroupStats(0, 0x1000)
        entry = mc.Entry()
        entry.count, entry.bytes, entry.aux, entry.frames = 1, 0x1000, 0x1000, 0b1
        stats.add((0, mc.UPLOAD, BIND["texture"], 0, 0x1000), entry, 1.0)
        self.assertEqual(stats.sums["unchanged_bytes"], 0)
        self.assertEqual(stats.frames["changed_upload"], 0b1)

    def test_report_and_json(self):
        out = io.StringIO()
        json_path = os.path.join(self.directory.name, "groups.json")
        with redirect_stdout(out):
            mc.main([self.path, "--frames", "1:3", "--json", json_path])
        report = out.getvalue()
        self.assertIn("3 frames", report)
        self.assertIn("Top groups by upload bytes", report)
        self.assertIn("gpu-written-cpu-read", report)
        self.assertIn("CPUCore_0", report)
        with open(json_path) as handle:
            data = json.load(handle)
        self.assertEqual(data["frames"], 3)
        read_back = next(g for g in data["groups"] if g["begin"] == READ_BACK)
        self.assertAlmostEqual(read_back["per_frame"]["wait_ns"], 500000)

    def test_marks_select_measure_span(self):
        marks = os.path.join(self.directory.name, "marks.json")
        with open(marks, "w") as handle:
            json.dump({"measure": 2, "end": 3}, handle)
        out = io.StringIO()
        with redirect_stdout(out):
            mc.main([self.path, "--marks", marks])
        self.assertIn("2 frames", out.getvalue())

    def test_unfinished_block_is_ignored(self):
        with open(self.path, "ab") as handle:
            handle.write(mc.BLOCK.pack(mc.BLOCK_MAGIC, mc.RECORDS, 0, 9, 3))
            handle.write(b"\0" * 10)
        census = mc.read_census(self.path)
        self.assertNotIn(9, census.frames)


if __name__ == "__main__":
    unittest.main()
