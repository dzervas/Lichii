// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/mem_census.h"

namespace MemCensus = Common::MemCensus;
using MemCensus::Bind;
using MemCensus::Event;

TEST_CASE("Memory census sums a range's events per event and detail",
          "[common][mem_census]") {
    MemCensus::FrameTable table;
    REQUIRE(table.Empty());
    const auto vertex = static_cast<u8>(Bind::Vertex);
    const auto uniform = static_cast<u8>(Bind::Uniform);
    table.Add(Event::Upload, vertex, 0x10000, 0x200, 0x200, 0, 0x200);
    table.Add(Event::Upload, vertex, 0x10000, 0x200, 0x100, 0, 0);
    table.Add(Event::Upload, uniform, 0x10000, 0x200, 0x40, 0, 0);
    table.Add(Event::Upload, vertex, 0x10000, 0x100, 0x100, 0, 0);
    table.Add(Event::CpuWrite, 0, 0x10000, 0x1000, 8, 5, 1);
    REQUIRE_FALSE(table.Empty());

    const std::vector<MemCensus::Record> records = table.Drain();
    REQUIRE(table.Empty());
    REQUIRE(records.size() == 4);
    // (event, detail, addr, size) order: CpuWrite first, then the uploads by bind and size.
    REQUIRE(records[0].event == static_cast<u8>(Event::CpuWrite));
    REQUIRE(records[0].count == 1);
    REQUIRE(records[0].bytes == 8);
    REQUIRE(records[0].ns == 5);
    REQUIRE(records[0].aux == 1);
    REQUIRE(records[1].detail == vertex);
    REQUIRE(records[1].size == 0x100);
    REQUIRE(records[1].count == 1);
    REQUIRE(records[2].detail == vertex);
    REQUIRE(records[2].size == 0x200);
    REQUIRE(records[2].count == 2);
    REQUIRE(records[2].bytes == 0x300);
    REQUIRE(records[2].aux == 0x200);
    REQUIRE(records[3].detail == uniform);
    REQUIRE(records[3].bytes == 0x40);
    REQUIRE(table.Drain().empty());
}

TEST_CASE("Memory census clamps range sizes to 32 bits", "[common][mem_census]") {
    MemCensus::FrameTable table;
    table.Add(Event::Protect, 0, 0, u64{1} << 40, 0, 0, 0);
    table.Add(Event::Protect, 0, 0, (u64{1} << 40) + 1, 0, 0, 0);
    const auto records = table.Drain();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].size == 0xffffffffU);
    REQUIRE(records[0].count == 2);
}

TEST_CASE("Memory census tells uploads of the same bytes apart", "[common][mem_census]") {
    MemCensus::UploadHistory history;
    const std::array<u8, 4> first{1, 2, 3, 4};
    const std::array<u8, 4> second{1, 2, 3, 5};
    const u64 a = MemCensus::Hash(first, 0);
    const u64 b = MemCensus::Hash(second, 0);
    REQUIRE(a != b);
    // The same bytes at another place of the upload hash differently.
    REQUIRE(MemCensus::Hash(first, 0x40) != a);

    REQUIRE_FALSE(history.Same(0x1000, 4, a));
    REQUIRE(history.Same(0x1000, 4, a));
    REQUIRE_FALSE(history.Same(0x1000, 4, b));
    REQUIRE(history.Same(0x1000, 4, b));
    // Another range, or the same address with another size, has its own history.
    REQUIRE_FALSE(history.Same(0x2000, 4, b));
    REQUIRE_FALSE(history.Same(0x1000, 8, b));
    REQUIRE(history.Size() == 3);
}

TEST_CASE("Memory census is off without its variable", "[common][mem_census]") {
    // ReadSwitch is only called by VideoCore::CreateGPU.
    REQUIRE_FALSE(MemCensus::g_enabled);
    MemCensus::SetBind(Bind::Vertex);
    REQUIRE(MemCensus::t_bind == Bind::None);
    {
        const MemCensus::ReasonScope scope{MemCensus::Reason::GuestRead};
        REQUIRE(MemCensus::t_reason == MemCensus::Reason::Other);
    }
}
