// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/sparse_large_vector.h"

namespace {

using Entry = Common::PageTable::PageEntryData;

const std::size_t EntriesPerHostPage = Common::HostPageSize / sizeof(Entry);

} // namespace

TEST_CASE("SparseLargeVector zeroes every entry of a region", "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 4);
    const std::size_t begin = EntriesPerHostPage / 2;
    const std::size_t end = EntriesPerHostPage * 3 + EntriesPerHostPage / 2;

    entries.CommitRegion(0, entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries.GetUnchecked(i).Store(false, Common::PageType::Memory, 1, 0x1000);
    }

    entries.ZeroRegion(begin, end);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool zeroed = i >= begin && i < end;
        REQUIRE((entries[i].Type() == Common::PageType::Unmapped) == zeroed);
    }
}

// A thread that loaded an entry before an unmap may still store to it afterwards, as
// RasterizerMarkRegionCached does when the GPU uncaches memory the guest is unmapping.
// That store must land in memory the vector still tracks, so the page stays writable and
// the next unmap clears it.
TEST_CASE("SparseLargeVector keeps zeroed pages writable for in-flight stores", "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 2);
    const std::size_t index = EntriesPerHostPage + 3;

    entries.CommitRegion(index, index + 1);
    Entry& entry = entries.GetUnchecked(index);
    entry.Store(false, Common::PageType::RasterizerCachedMemory, 1, 0x1000);

    entries.ZeroRegion(EntriesPerHostPage, EntriesPerHostPage * 2);
    REQUIRE(entries[index].Type() == Common::PageType::Unmapped);

    entry.Store(false, Common::PageType::Memory, 1, 0x1000);
    REQUIRE(entries[index].Type() == Common::PageType::Memory);

    entries.ZeroRegion(EntriesPerHostPage, EntriesPerHostPage * 2);
    REQUIRE(entries[index].Type() == Common::PageType::Unmapped);
}

TEST_CASE("SparseLargeVector reads zero where nothing was written", "[common]") {
    // 1 GiB, as the page table of a 39-bit address space.
    Common::SparseLargeVector<u64> values(std::size_t{1} << 27);
    const std::size_t far = values.size() - 1;

    REQUIRE(values[0] == 0);
    REQUIRE(values[far] == 0);

    values.Set(far, 1);
    values.Set(values.size() / 2, 2);
    REQUIRE(values[far] == 1);
    REQUIRE(values[values.size() / 2] == 2);
    REQUIRE(values[far - 1] == 0);
    REQUIRE(values[values.size() / 2 + 1] == 0);
    REQUIRE(values[0] == 0);
}

TEST_CASE("SparseLargeVector writes pages that were read before", "[common]") {
    Common::SparseLargeVector<u64> values(Common::HostPageSize / sizeof(u64) * 4);
    const std::size_t per_page = Common::HostPageSize / sizeof(u64);

    REQUIRE(values[per_page] == 0);
    REQUIRE(values[per_page * 2] == 0);
    REQUIRE(values[per_page * 3] == 0);

    values.Set(per_page + 1, 3);
    values.GetAndFault(per_page * 2) = 4;
    values.CommitRegion(per_page * 3, per_page * 4);
    values.GetUnchecked(per_page * 3) = 5;
    REQUIRE(values[per_page + 1] == 3);
    REQUIRE(values[per_page * 2] == 4);
    REQUIRE(values[per_page * 3] == 5);

    values.ZeroRegion(0, values.size());
    REQUIRE(values[per_page + 1] == 0);
    REQUIRE(values[per_page * 2] == 0);
    REQUIRE(values[per_page * 3] == 0);
}

// Readers of a page may race the thread that commits it with the first write. A stress test: the
// reader's first sweep of the unread pages starts with the writes; how they interleave is timing.
TEST_CASE("SparseLargeVector reads pages another thread is writing", "[common]") {
    constexpr std::size_t Pages = 1024;
    for (int round = 0; round < 8; ++round) {
        Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * Pages);
        std::atomic<bool> reading{};
        std::atomic<bool> done{};
        bool read_other{};

        std::thread reader([&] {
            reading.store(true, std::memory_order_release);
            do {
                for (std::size_t page = 0; page < Pages; ++page) {
                    const auto type = entries[page * EntriesPerHostPage].Type();
                    read_other |=
                        type != Common::PageType::Unmapped && type != Common::PageType::Memory;
                }
            } while (!done.load(std::memory_order_acquire));
        });
        while (!reading.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t page = 0; page < Pages; ++page) {
            entries.GetAndFault(page * EntriesPerHostPage)
                .Store(false, Common::PageType::Memory, 1, 0x1000);
        }
        done.store(true, std::memory_order_release);
        reader.join();

        REQUIRE_FALSE(read_other);
        for (std::size_t page = 0; page < Pages; ++page) {
            REQUIRE(entries[page * EntriesPerHostPage].Type() == Common::PageType::Memory);
            entries.GetAndFault(page * EntriesPerHostPage)
                .Store(false, Common::PageType::Unmapped, 0, 0);
        }
    }
}
