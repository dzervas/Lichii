// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_MEM_CENSUS=<file>: what keeping the CPU's and the GPU's copies of guest memory coherent
// costs, per guest range and per frame (a frame ends at each present). Each thread sums its
// events per (event, detail, range) in its own table and hands the table to a writer thread when
// it records its first event of a later frame (or exits), so recording takes no lock shared with
// other threads. Ranges are device addresses: binding ranges for buffer uploads and GPU writes,
// 4 KiB pages for CPU writes, page runs for protection changes, images for the texture cache.
// tools/nx2/mem_census.py reads the file and groups the ranges.
namespace Common::MemCensus {

// tools/nx2/mem_census.py mirrors these values.
enum class Event : u8 {
    CpuWrite,  ///< A guest write to GPU-cached memory (detail: CpuWrite)
    Protect,   ///< Pages made write-protected (detail: Protection)
    Unprotect, ///< Pages made writable again (detail: Protection)
    Upload,    ///< Guest memory copied to the host GPU (detail: Bind; aux: unchanged bytes)
    GpuWrite,  ///< A range the GPU writes, marked GPU-modified (detail: Bind)
    Download,  ///< GPU bytes copied back to guest memory (detail: Reason)
    ReadWait,  ///< A thread waited for downloads (detail: Reason; ns: the wait)
};

enum class CpuWrite : u8 {
    Trap,       ///< Each write that reached Memory's rasterizer write path
    Invalidate,   ///< ...that called the GPU caches' OnCPUWrite (ns; aux: kept collected)
    InlineEngine, ///< Inline-to-memory engine data written by the GPU thread, per page (aux:
                  ///< uploaded into GPU-written memory)
};

enum class Protection : u8 {
    Direct,   ///< Made by the thread that changed the cache counts
    Deferred, ///< Unprotected after the caches' mutexes (ScopedUncacheDeferral)
};

/// How the GPU uses the range: the bind that uploaded it or the operation that wrote it.
enum class Bind : u8 {
    None,
    Vertex,
    Index,
    Indirect,
    Uniform,              ///< Graphics uniform buffer through the cache (page protection)
    UniformStream,        ///< Small graphics uniform streamed at bind (no protection)
    UniformCompute,
    UniformStreamCompute,
    StorageGraphics,
    StorageCompute,
    TextureBuffer,
    TextureBufferCompute,
    TransformFeedback,
    Dma,        ///< DMA engine copy (upload of its source, GPU write of its destination)
    DmaClear,
    Inline,     ///< Inline engine or CPU bytes written into GPU-written buffer memory
    Obtain,     ///< Buffers the rasterizer asks for directly (queries, indirect, macros)
    NativeBank, ///< 64 KiB native uniform bank uploaded again each frame
    Texture,    ///< Texture cache image (aux of uploads: not hashed)
    Image,      ///< Texture cache image marked GPU-modified (render target, copy, storage)
};

enum class Reason : u8 {
    Other,           ///< A flush with no reason set (memory manager reads, engines)
    GuestRead,       ///< A guest CPU read of GPU-written memory
    EarlyRead,       ///< Downloaded ahead of an expected guest read
    CommandList,     ///< GPU-written command list pages
    Query,           ///< Query and report results
    MacroRead,       ///< Macro parameters read by DMA
    Flush,           ///< Asynchronous flushes at fences
    TextureGc,       ///< Images downloaded before the texture cache deletes them
    CpuWriteOverGpu, ///< A guest write over GPU-written memory
};

/// One (event, detail, range)'s sums over a frame on one thread; the file's record layout.
struct Record {
    u64 addr;
    u64 bytes;
    u64 ns;
    u64 aux;
    u32 size; ///< Clamped to 4 GiB - 1
    u32 count;
    u8 event;
    u8 detail;
    u8 padding[6];
};
static_assert(sizeof(Record) == 48);

/// A thread's sums for the frame it is recording.
class FrameTable {
public:
    void Add(Event event, u8 detail, u64 addr, u64 size, u64 bytes, u64 ns, u64 aux);
    [[nodiscard]] bool Empty() const {
        return records.empty();
    }
    /// The records in (event, detail, addr, size) order; empties the table.
    std::vector<Record> Drain();

private:
    struct Key {
        u64 addr;
        u32 size;
        u8 event;
        u8 detail;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept;
    };
    std::unordered_map<Key, Record, KeyHash> records;
};

/// The last uploaded bytes' hash per range: whether an upload brought the same bytes again.
class UploadHistory {
public:
    /// Remembers `hash` for the range; true when its previous upload had the same hash.
    bool Same(u64 addr, u64 size, u64 hash);
    [[nodiscard]] size_t Size() const {
        return hashes.size();
    }

    /// Ranges remembered before the history starts over.
    static constexpr size_t MaxRanges = size_t{1} << 20;

private:
    struct Key {
        u64 addr;
        u64 size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept;
    };
    std::unordered_map<Key, u64, KeyHash> hashes;
};

/// Whether the census records (a plain flag: tested on every instrumented path).
inline bool g_enabled{};

/// The bind or GPU operation the calling thread is synchronizing buffers for (set while enabled).
inline thread_local Bind t_bind{};
/// Why the calling thread downloads (set while enabled).
inline thread_local Reason t_reason{};

/// Called by VideoCore::CreateGPU: opens EDEN_NX2_MEM_CENSUS's file once.
void ReadSwitch();

/// Ends the current frame (RendererVulkan::Composite). Only called while g_enabled.
void Present();

/// Adds an event to the calling thread's table. Only called while g_enabled.
void Note(Event event, u8 detail, u64 addr, u64 size, u64 bytes, u64 ns = 0, u64 aux = 0);

/// An upload of `bytes` for the range [addr, addr + size), `hash` of what was uploaded (Hash):
/// aux counts the bytes when the range's previous upload on this thread had the same hash.
void NoteUpload(Bind bind, u64 addr, u64 size, u64 bytes, u64 hash);

/// Hash of uploaded bytes, chained over an upload's copies through `seed`.
u64 Hash(std::span<const u8> data, u64 seed);

inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void SetBind(Bind bind) {
    if (g_enabled) [[unlikely]] {
        t_bind = bind;
    }
}

/// Times a wait for downloads: Done notes a ReadWait with the calling thread's reason.
class WaitTimer {
public:
    WaitTimer() {
        if (g_enabled) [[unlikely]] {
            start = Now();
        }
    }
    void Done(u64 addr, u64 size) const {
        if (g_enabled) [[unlikely]] {
            Note(Event::ReadWait, static_cast<u8>(t_reason), addr, size, size, Now() - start);
        }
    }

private:
    u64 start{};
};

/// Sets the calling thread's download reason for its lifetime.
class ReasonScope {
public:
    explicit ReasonScope(Reason reason) {
        if (g_enabled) [[unlikely]] {
            previous = t_reason;
            t_reason = reason;
            active = true;
        }
    }
    ~ReasonScope() {
        if (active) {
            t_reason = previous;
        }
    }
    ReasonScope(const ReasonScope&) = delete;
    ReasonScope& operator=(const ReasonScope&) = delete;

private:
    Reason previous{};
    bool active{};
};

} // namespace Common::MemCensus
