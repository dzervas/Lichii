// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Lichii: a pool mirror's copy rules per word and per page (vka_mem.c "Mirrors"), as the shaders apply them (vka_mirror_spv.h: mirror_back.comp, mirror_in.comp).
// Plain C for the model test (src/tests/video_core/nvn_mirror_model.c); vka_mem.c records the shaders
#pragma once
#include <stdint.h>

#define VKM_ST_UNARMED 0x80000000u   // nvn_dirty.h NVN_DIRTY_ST_*
#define VKM_ST_ACTIVE 0x40000000u
#define VKM_ST_COUNT 0x3fffffffu

// The bytes of a word in which d (an XOR of two words) has a bit set
static inline uint32_t vkm_bytes(uint32_t d) {
    return (d & 0xffu ? 0xffu : 0u) | (d & 0xff00u ? 0xff00u : 0u) | (d & 0xff0000u ? 0xff0000u : 0u) | (d & 0xff000000u ? 0xff000000u : 0u);
}
// Publish (mirror_back.comp): the bytes the owner changed since the last agreement, from the mirror into guest memory (merged into its other bytes when the change is
// partial: the shader's compare-and-swap)
static inline void vkm_publish_word(uint32_t mir, uint32_t *shadow, uint32_t *guest) {
    if (mir == *shadow) return;
    const uint32_t m = vkm_bytes(mir ^ *shadow);
    *guest = m == 0xffffffffu ? mir : (*guest & ~m) | (mir & m);
    *shadow = mir;
}
// Acquire (mirror_in.comp): the bytes of guest memory that changed since the last agreement, into the mirror
static inline void vkm_acquire_word(uint32_t guest, uint32_t *shadow, uint32_t *mir) {
    if (guest == *shadow) return;
    const uint32_t m = vkm_bytes(guest ^ *shadow);
    *mir = (*mir & ~m) | (guest & m);
    *shadow = guest;
}
// Whether the acquire copies a page: its owner is the queue, and it is not armed, or its count moved since the owner last saw it
static inline int vkm_acquire_page(uint32_t state, uint32_t seen_count, uint32_t page_queue, uint32_t queue) {
    return (state & VKM_ST_ACTIVE) && page_queue == queue && ((state & VKM_ST_UNARMED) || (state & VKM_ST_COUNT) != seen_count);
}
