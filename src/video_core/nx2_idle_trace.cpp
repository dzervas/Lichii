// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

#include "common/logging.h"
#include "video_core/nx2_idle_trace.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon::IdleTrace {
namespace {

// File layout, little endian: blocks of a 16-byte header {u32 magic, u32 thread, u32 count,
// u32 reserved} and `count` 16-byte records. Event records: {u64 ticks, u32 arg, u16 event,
// u16 aux}, ticks from Nx2Profile::Ticks (GPU ns for the Gpu* events). A thread's first block has count 0 and its name (16
// bytes) instead. Calibration blocks (thread CalibrationThread) hold one {u64 ticks, u64 steady
// clock ns} pair.
constexpr u32 Magic = 0x544c4449; // "IDLT"
constexpr u32 CalibrationThread = ~0U;
constexpr size_t ChunkRecords = 8192;
/// The writer stops past this much (a run left tracing for hours).
constexpr u64 MaxBytes = u64{4} << 30;

struct Record {
    u64 ticks;
    u32 arg;
    u16 event;
    u16 aux;
};
static_assert(sizeof(Record) == 16);

struct Header {
    u32 magic;
    u32 thread;
    u32 count;
    u32 reserved;
};

struct Chunk {
    u32 thread{};
    u32 count{};
    std::array<Record, ChunkRecords> records;
};

// Never destroyed: threads may still hand chunks over while the process exits. Each batch is
// flushed to the file as it is written.
class Writer {
public:
    bool Open(const char* path) {
        file = std::fopen(path, "wb");
        if (file == nullptr) {
            return false;
        }
        Calibrate();
        std::thread([this] { Run(); }).detach();
        return true;
    }

    u32 Register() {
        std::array<char, 16> name{};
#ifdef __linux__
        pthread_getname_np(pthread_self(), name.data(), name.size());
#endif
        std::scoped_lock lock{mutex};
        const u32 index = next_thread++;
        names.emplace_back(index, name);
        return index;
    }

    /// Hands a full (or a finished thread's last) chunk over; returns an empty one.
    std::unique_ptr<Chunk> Swap(std::unique_ptr<Chunk> full, u32 thread_index) {
        std::unique_ptr<Chunk> empty;
        {
            std::scoped_lock lock{mutex};
            if (full != nullptr) {
                queue.push_back(std::move(full));
            }
            if (!pool.empty()) {
                empty = std::move(pool.back());
                pool.pop_back();
            }
        }
        ready.notify_one();
        if (empty == nullptr) {
            empty = std::make_unique<Chunk>();
        }
        empty->thread = thread_index;
        empty->count = 0;
        return empty;
    }

    void Retire(std::unique_ptr<Chunk> last) {
        std::scoped_lock lock{mutex};
        queue.push_back(std::move(last));
        ready.notify_one();
    }

private:
    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            ready.wait(lock, [this] { return !queue.empty() || !names.empty(); });
            const auto pending_names = std::exchange(names, {});
            std::deque<std::unique_ptr<Chunk>> chunks;
            chunks.swap(queue);
            lock.unlock();
            for (const auto& [index, name] : pending_names) {
                Put(Header{Magic, index, 0, 0}, name.data(), name.size());
            }
            for (auto& chunk : chunks) {
                Put(Header{Magic, chunk->thread, chunk->count, 0}, chunk->records.data(),
                    chunk->count * sizeof(Record));
            }
            Calibrate();
            std::fflush(file);
            lock.lock();
            for (auto& chunk : chunks) {
                pool.push_back(std::move(chunk));
            }
        }
    }

    void Calibrate() {
        const u64 ticks = Nx2Profile::Ticks();
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
        const std::array<u64, 2> pair{ticks, ns};
        Put(Header{Magic, CalibrationThread, 1, 0}, pair.data(), sizeof(pair));
    }

    void Put(const Header& header, const void* data, size_t size) {
        if (written >= MaxBytes) {
            return;
        }
        std::fwrite(&header, sizeof(header), 1, file);
        std::fwrite(data, 1, size, file);
        written += sizeof(header) + size;
        if (written >= MaxBytes) {
            LOG_WARNING(HW_GPU, "Idle trace: {} GiB written, stopped", MaxBytes >> 30);
        }
    }

    std::FILE* file{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::unique_ptr<Chunk>> queue;
    std::vector<std::unique_ptr<Chunk>> pool;
    std::vector<std::pair<u32, std::array<char, 16>>> names;
    u32 next_thread{};
    u64 written{};
};

Writer& g_writer = *new Writer;

/// The calling thread's chunk; a thread that exits hands its last records to the writer.
struct ThreadChunk {
    ~ThreadChunk() {
        if (chunk != nullptr && chunk->count != 0) {
            g_writer.Retire(std::move(chunk));
        }
    }
    std::unique_ptr<Chunk> chunk;
    u32 index{};
};

thread_local ThreadChunk t_chunk;

} // Anonymous namespace

void ReadSwitch() {
    if (g_enabled) {
        return;
    }
    const char* const path = std::getenv("EDEN_NX2_IDLE_TRACE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    if (!g_writer.Open(path)) {
        LOG_ERROR(HW_GPU, "Idle trace: can't open {}", path);
        return;
    }
    LOG_INFO(HW_GPU, "Idle trace: writing to {}", path);
    g_enabled = true;
}

u16 FileId(const char* file_name) {
    static std::mutex mutex;
    static std::vector<const char*> names;
    std::scoped_lock lock{mutex};
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == file_name || std::strcmp(names[i], file_name) == 0) {
            return static_cast<u16>(i);
        }
    }
    names.push_back(file_name);
    LOG_INFO(HW_GPU, "Idle trace: file id {} = {}", names.size() - 1, file_name);
    return static_cast<u16>(names.size() - 1);
}

void Write(Event event, u16 aux, u32 arg) {
    WriteAt(Nx2Profile::Ticks(), event, aux, arg);
}

void WriteAt(u64 time, Event event, u16 aux, u32 arg) {
    auto& own = t_chunk;
    if (own.chunk == nullptr) {
        own.index = g_writer.Register();
        own.chunk = g_writer.Swap(nullptr, own.index);
    } else if (own.chunk->count == ChunkRecords) {
        own.chunk = g_writer.Swap(std::move(own.chunk), own.index);
    }
    own.chunk->records[own.chunk->count++] = Record{time, arg, static_cast<u16>(event), aux};
}

} // namespace VideoCommon::IdleTrace
