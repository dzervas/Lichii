// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// nvn-probe (dotfiles-2hg.62) diagnostic: see OriginProfile in nx2_profile.h.

#include <memory>

#include <fmt/format.h>

#include "video_core/nx2_profile.h"

namespace VideoCommon::OriginProfile {

Tables& GetTables() {
    static const std::unique_ptr<Tables> tables = std::make_unique<Tables>();
    return *tables;
}

void Dump(std::string& out, u64 present) {
    if (!g_enabled) {
        return;
    }
    auto& tables = GetTables();
    static bool names_written = false;
    if (!names_written) {
        names_written = true;
        for (size_t z = 0; z < Nx2Profile::zone_names.size(); ++z) {
            out += fmt::format("zn {} {}\n", z, Nx2Profile::zone_names[z]);
        }
    }
    const u32 ids = (std::min)(tables.next_id, MaxOrigins);
    for (u32 role = 0; role < static_cast<u32>(Role::Count); ++role) {
        for (u32 zone = 0; zone < MaxZones; ++zone) {
            for (u32 id = 0; id < ids; ++id) {
                const u64 ticks = tables.ns[role][zone][id];
                if (ticks != 0) {
                    out += fmt::format("o {} {} {} {:#x} {}\n", present, role, zone,
                                       id == 0 ? 0xffffffffu : tables.id_to_key[id], ticks);
                }
            }
        }
    }
    for (u32 id = 1; id < ids; ++id) {
        if (tables.headers[id] != 0) {
            out += fmt::format("oh {} {:#x} {}\n", present, tables.id_to_key[id], tables.headers[id]);
        }
    }
    for (u32 kind = 0; kind < 2; ++kind) {
        for (u32 cls = 0; cls < 2; ++cls) {
            for (u32 bucket = 0; bucket < Buckets; ++bucket) {
                const u64 count = tables.bucket_count[kind][cls][bucket];
                if (count == 0) {
                    continue;
                }
                out += fmt::format("dc {} {} {} {} {} {}\n", present, kind, cls, bucket, count,
                                   tables.bucket_binds[kind][cls][bucket]);
                for (u32 zone = 0; zone < MaxZones; ++zone) {
                    const u64 ticks = tables.bucket_ns[kind][cls][bucket][zone];
                    if (ticks != 0) {
                        out += fmt::format("db {} {} {} {} {} {}\n", present, kind, cls, bucket,
                                           zone, ticks);
                    }
                }
            }
        }
    }
}

} // namespace VideoCommon::OriginProfile
