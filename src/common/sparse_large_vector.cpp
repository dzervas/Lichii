// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.cpp */
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <windows.h>
#include <mutex>
#else
#include <sys/mman.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"
#include "common/sparse_large_vector.h"

namespace Common {

#ifdef _WIN32
static std::vector<std::pair<u64, u64>> vector_regions {};
static std::mutex vector_regions_mutex {};

// The first read of a page nothing has written yet (GetOrDefault, Dynarmic's page table lookups)
// faults: commit the page, which reads as zero, and retry. Writes must commit through the vector.
static LONG WINAPI FakePageFaultHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    u64 exception_addr = static_cast<u64>(info->ExceptionRecord->ExceptionInformation[1]);

    if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionInformation[0] == 1) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    u64 addr = 0, addr2 = 0;

    {
        std::lock_guard lock(vector_regions_mutex);
        for (auto region: vector_regions) {
            auto addr_shifted = exception_addr >> HostPageBits;
            if (region.first <= addr_shifted && addr_shifted < region.second) {
                addr = addr_shifted;
            }

            // Page-boundary accesses
            if (auto addr_ = (exception_addr + 0x40) >> HostPageBits; addr_ != addr_shifted && region.first <= addr_ && addr_ < region.second) {
                addr2 = addr_;
            }

            if (addr != 0 || addr2 != 0) {
                break;
            }
        }
    }

    if (addr == 0 && addr2 == 0) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Commit this region
    if (addr != 0) {
        if (!CommitVectorPage(addr << HostPageBits)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    // Commit next region if needed
    if (addr2 != 0) {
        if (!CommitVectorPage(addr2 << HostPageBits)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// Pages are always committed read-write, as a read fault and a write on another thread may commit
// the same page at once. Committing a committed page succeeds and keeps its contents.
bool CommitVectorPage(uintptr_t addr) noexcept {
    if (!VirtualAlloc(reinterpret_cast<LPVOID>(addr), HostPageSize, MEM_COMMIT, PAGE_READWRITE)) {
        LOG_ERROR(HW_Memory, "Failed to commit large buffer region at {:#x}, error {}", addr, GetLastError());
        return false;
    }
    return true;
}
#endif

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

void* AllocateMemoryPages(std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        LOG_WARNING(HW_Memory, "Allocating unaligned large vector with size {:#x}; aligning to {} page size", size, page);
        size = AlignUp(size, page);
    }

#ifdef _WIN32
    // We will never use this memory entirely so instead of committing it up front let's just reserve it and commit each page individually
    void* base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);

    if (base != nullptr) {
        std::lock_guard lock(vector_regions_mutex);
        vector_regions.emplace_back(reinterpret_cast<u64>(base) >> HostPageBits, (reinterpret_cast<u64>(base) + size) >> HostPageBits);

        static std::once_flag flag;
        std::call_once(flag, []() { AddVectoredExceptionHandler(1, FakePageFaultHandler); });
    } else {
        // Try committing everything instead??
        LOG_WARNING(HW_Memory, "Failed to reserve large vector region with error {}, trying to commit instead..", GetLastError());
        base = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
    }
    ASSERT_MSG(base, "Failed to reserve {:#x} sized region with error {}", size, GetLastError());
#else
    void* base = mmap(nullptr, size, PROT_READ, MAP_ANON | MAP_PRIVATE | MAP_NOCORE, -1, 0);
    if (base == MAP_FAILED)
        base = nullptr;
    ASSERT_MSG(base, "Failed to allocate {:#x} sized region with error {}", size, strerror(errno));
#endif
    return base;
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        size = AlignUp(size, page);
    }
    if (!base)
        return;
#ifdef _WIN32
    std::lock_guard lock(vector_regions_mutex);
    // Regions are kept in host pages.
    std::erase_if(vector_regions, [base](const auto& r) {return r.first == reinterpret_cast<u64>(base) >> HostPageBits; });
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common
