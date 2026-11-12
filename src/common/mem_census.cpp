// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>

#ifdef __linux__
#include <pthread.h>
#endif

#include "common/cityhash.h"
#include "common/logging.h"
#include "common/mem_census.h"

namespace Common::MemCensus {

namespace {

constexpr u64 Mix(u64 value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return value;
}

} // Anonymous namespace

size_t FrameTable::KeyHash::operator()(const Key& key) const noexcept {
    return static_cast<size_t>(
        Mix(key.addr ^ (u64{key.size} << 20) ^ (u64{key.event} << 56) ^ (u64{key.detail} << 48)));
}

void FrameTable::Add(Event event, u8 detail, u64 addr, u64 size, u64 bytes, u64 ns, u64 aux) {
    const u32 clamped = static_cast<u32>((std::min<u64>)(size, std::numeric_limits<u32>::max()));
    const Key key{addr, clamped, static_cast<u8>(event), detail};
    auto [it, inserted] = records.try_emplace(key);
    Record& record = it->second;
    if (inserted) {
        record.addr = addr;
        record.size = clamped;
        record.event = static_cast<u8>(event);
        record.detail = detail;
    }
    ++record.count;
    record.bytes += bytes;
    record.ns += ns;
    record.aux += aux;
}

std::vector<Record> FrameTable::Drain() {
    std::vector<Record> out;
    out.reserve(records.size());
    for (const auto& [key, record] : records) {
        out.push_back(record);
    }
    records.clear();
    std::ranges::sort(out, [](const Record& lhs, const Record& rhs) {
        return std::tie(lhs.event, lhs.detail, lhs.addr, lhs.size) <
               std::tie(rhs.event, rhs.detail, rhs.addr, rhs.size);
    });
    return out;
}

size_t UploadHistory::KeyHash::operator()(const Key& key) const noexcept {
    return static_cast<size_t>(Mix(key.addr ^ (key.size << 40)));
}

bool UploadHistory::Same(u64 addr, u64 size, u64 hash) {
    if (hashes.size() >= MaxRanges) {
        hashes.clear();
    }
    auto [it, inserted] = hashes.try_emplace(Key{addr, size}, hash);
    if (inserted) {
        return false;
    }
    const bool same = it->second == hash;
    it->second = hash;
    return same;
}

namespace {

// File layout, little endian: a 16-byte header ("MEMCENS1", u32 record size, u32 0), then
// blocks of a 16-byte BlockHeader and their payload: a thread's name (16 bytes), `count` Records
// of `frame` from `thread`, or a present's steady clock ns (u64) ending `frame`.
constexpr u32 BlockMagic = 0x4e45434d; // "MCEN"
constexpr u64 MaxBytes = u64{8} << 30;

enum class BlockType : u16 {
    ThreadName,
    Records,
    Present,
};

struct BlockHeader {
    u32 magic;
    u16 type;
    u16 thread;
    u32 frame;
    u32 count;
};
static_assert(sizeof(BlockHeader) == 16);

struct Block {
    BlockHeader header;
    std::vector<Record> records;
    std::array<char, 16> name{};
    u64 present_ns{};
};

std::atomic<u32> g_frame{};

// Never destroyed: threads may still hand tables over while the process exits.
class Writer {
public:
    bool Open(const char* path) {
        file = std::fopen(path, "wb");
        if (file == nullptr) {
            return false;
        }
        const std::array<u32, 4> header{0x434d454d, 0x31534e45, sizeof(Record), 0}; // "MEMCENS1"
        std::fwrite(header.data(), sizeof(header), 1, file);
        std::thread([this] { Run(); }).detach();
        return true;
    }

    u16 Register() {
        Block block{};
        block.header = {BlockMagic, static_cast<u16>(BlockType::ThreadName), 0, 0, 0};
#ifdef __linux__
        pthread_getname_np(pthread_self(), block.name.data(), block.name.size());
#endif
        std::scoped_lock lock{mutex};
        const u16 index = next_thread++;
        block.header.thread = index;
        queue.push_back(std::move(block));
        return index;
    }

    void Push(u16 thread, u32 frame, std::vector<Record> records) {
        Block block{};
        block.header = {BlockMagic, static_cast<u16>(BlockType::Records), thread, frame,
                        static_cast<u32>(records.size())};
        block.records = std::move(records);
        Enqueue(std::move(block));
    }

    void Present(u32 frame, u64 ns) {
        Block block{};
        block.header = {BlockMagic, static_cast<u16>(BlockType::Present), 0, frame, 0};
        block.present_ns = ns;
        Enqueue(std::move(block));
    }

private:
    void Enqueue(Block&& block) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back(std::move(block));
        }
        ready.notify_one();
    }

    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            ready.wait(lock, [this] { return !queue.empty(); });
            std::deque<Block> blocks;
            blocks.swap(queue);
            lock.unlock();
            for (const Block& block : blocks) {
                Put(block);
            }
            std::fflush(file);
            lock.lock();
        }
    }

    void Put(const Block& block) {
        if (written >= MaxBytes) {
            return;
        }
        std::fwrite(&block.header, sizeof(block.header), 1, file);
        written += sizeof(block.header);
        switch (static_cast<BlockType>(block.header.type)) {
        case BlockType::ThreadName:
            std::fwrite(block.name.data(), block.name.size(), 1, file);
            written += block.name.size();
            break;
        case BlockType::Records:
            std::fwrite(block.records.data(), sizeof(Record), block.records.size(), file);
            written += sizeof(Record) * block.records.size();
            break;
        case BlockType::Present:
            std::fwrite(&block.present_ns, sizeof(block.present_ns), 1, file);
            written += sizeof(block.present_ns);
            break;
        }
        if (written >= MaxBytes) {
            LOG_WARNING(Common, "Memory census: {} GiB written, stopped", MaxBytes >> 30);
        }
    }

    std::FILE* file{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Block> queue;
    u16 next_thread{};
    u64 written{};
};

Writer& g_writer = *new Writer;

/// The calling thread's table; a thread that exits hands its last one to the writer.
struct ThreadState {
    ThreadState() : index{g_writer.Register()} {}
    ~ThreadState() {
        Hand();
    }

    void Hand() {
        if (!table.Empty()) {
            g_writer.Push(index, frame, table.Drain());
        }
    }

    FrameTable table;
    UploadHistory history;
    u32 frame{};
    u16 index;
};

ThreadState& State() {
    thread_local ThreadState state;
    const u32 frame = g_frame.load(std::memory_order_relaxed);
    if (state.frame != frame) {
        state.Hand();
        state.frame = frame;
    }
    return state;
}

} // Anonymous namespace

void ReadSwitch() {
    if (g_enabled) {
        return;
    }
    const char* const path = std::getenv("EDEN_NX2_MEM_CENSUS");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    if (!g_writer.Open(path)) {
        LOG_ERROR(Common, "Memory census: can't open {}", path);
        return;
    }
    LOG_INFO(Common, "Memory census: writing to {}", path);
    g_enabled = true;
}

void Present() {
    const u32 frame = g_frame.fetch_add(1, std::memory_order_relaxed);
    g_writer.Present(frame, Now());
}

void Note(Event event, u8 detail, u64 addr, u64 size, u64 bytes, u64 ns, u64 aux) {
    State().table.Add(event, detail, addr, size, bytes, ns, aux);
}

void NoteUpload(Bind bind, u64 addr, u64 size, u64 bytes, u64 hash) {
    ThreadState& state = State();
    const bool same = state.history.Same(addr, size, hash);
    state.table.Add(Event::Upload, static_cast<u8>(bind), addr, size, bytes, 0, same ? bytes : 0);
}

u64 Hash(std::span<const u8> data, u64 seed) {
    return Common::CityHash64WithSeed(reinterpret_cast<const char*>(data.data()), data.size(),
                                      seed);
}

} // namespace Common::MemCensus
