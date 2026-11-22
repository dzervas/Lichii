// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A pool mirror's protocol (vka_mem.c "Mirrors") as a deterministic model: one page of a segment
// that only its owner queue Q touches (the mirror M and its shadow S), guest memory I that the CPU
// writes, and the copy rules of the shaders (vka_mirror_words.h). Each scenario runs the guest's
// ordered steps in one interleaving the replay allows: Q's copies happen at Q's release points
// (event signals, submission end, before a cooperative-vector conversion: publish) and acquire
// points (submission start, waits met or not, after a conversion: acquire), and the CPU's writes
// move the tracker's page state. Another queue's access to the segment never meets a mirror (it is
// never made, or it stops and is waited for: mir_touch), so the model has no second queue on the
// page. The scenarios are named after the reviews' findings (review 2 #5 and review 3 #1, #5 are
// code: pack_cand, mir_touch) and the cases the owner asked for; each checks what a sequentially
// consistent memory would hold.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vka_mirror_words.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); exit(1); } } while (0)

enum { N = 8, Q = 1 };
typedef struct {
    uint32_t m[N], s[N], i[N]; // mirror, shadow, guest memory
    uint32_t state;            // the tracker's page state (nvn_dirty_watch)
    uint32_t seen;             // the owner's seen count
    int q_wrote;               // written since Q's last release
} page_t;

static void init(page_t* g, const uint32_t* guest) {
    memset(g, 0, sizeof *g);
    memcpy(g->i, guest, sizeof g->i);
    memcpy(g->m, g->i, sizeof g->m); // mir_make: the mirror from guest memory,
    g->state = VKM_ST_ACTIVE | 1;
}
static void init_shadow(page_t* g) { memcpy(g->s, g->m, sizeof g->s); } // then the shadow from the mirror
static void cpu_write(page_t* g, int w, uint32_t v) { g->i[w] = v; g->state = VKM_ST_ACTIVE | VKM_ST_UNARMED | ((g->state + 1) & VKM_ST_COUNT); }
static void cpu_write_byte(page_t* g, int w, int b, uint8_t v) { cpu_write(g, w, (g->i[w] & ~(0xffu << 8 * b)) | (uint32_t)v << 8 * b); }
static void rearm(page_t* g) { if (g->state & VKM_ST_UNARMED) g->state = VKM_ST_ACTIVE | ((g->state + 1) & VKM_ST_COUNT); }
static void q_write(page_t* g, int w, uint32_t v) { g->m[w] = v; g->q_wrote = 1; }
static void q_write_byte(page_t* g, int w, int b, uint8_t v) { g->m[w] = (g->m[w] & ~(0xffu << 8 * b)) | (uint32_t)v << 8 * b; g->q_wrote = 1; }
static void q_release(page_t* g) { // mir_release: publish
    if (g->q_wrote) for (int w = 0; w < N; w++) vkm_publish_word(g->m[w], &g->s[w], &g->i[w]);
    g->q_wrote = 0;
}
static void q_acquire(page_t* g) { // mir_acquire
    if (!vkm_acquire_page(g->state, g->seen, Q, Q)) return;
    const uint32_t count = g->state & VKM_ST_COUNT;
    for (int w = 0; w < N; w++) vkm_acquire_word(g->i[w], &g->s[w], &g->m[w]);
    g->seen = count;
}
static void q_start(page_t* g) { rearm(g); q_acquire(g); } // vka_mem_sub_begin
static void fresh(page_t* g) { // a mirror made over zeros, at the owner's first submission
    static const uint32_t zero[N];
    init(g, zero);
    init_shadow(g);
    q_start(g);
}

// Review 2 #2: the CPU writes X = 1 before the submission; Q copies it in and writes X = 0. Only Q
// copies into its mirror, in order with its writes.
static void review2_f2_copyin_aba(void) {
    page_t g; fresh(&g);
    cpu_write(&g, 0, 1);
    q_start(&g);
    q_write(&g, 0, 0);
    q_release(&g);
    CHECK(g.m[0] == 0 && g.i[0] == 0);
}

// Review 2 #3: Q writes X = 1 and its submission ends; the CPU waits for the fence, writes X = 0
// (the old shadow value) and signals C; Q's next submission waits C and reads X.
static void review2_f3_cpu_rewrites_shadow_value(void) {
    page_t g; fresh(&g);
    q_write(&g, 0, 1); q_release(&g);  // submission end: published, S = 1
    q_start(&g);                       // the next submission, recorded before the CPU's write
    cpu_write(&g, 0, 0);               // CPU signals C
    q_acquire(&g);                     // Q waits C
    CHECK(g.m[0] == 0);
}

// Review 2 #4: a CPU write between the copies that make the mirror and the shadow. The shadow comes
// from the mirror, so both hold the same snapshot and the tracker's state brings the write in.
static void review2_f4_one_snapshot(void) {
    static const uint32_t zero[N];
    page_t g; init(&g, zero);
    cpu_write(&g, 3, 9);               // between the two copies
    init_shadow(&g);
    q_start(&g);
    CHECK(g.m[3] == 9 && g.s[3] == 9);
}

// Review 3 #2: Q's submission starts (acquire), the CPU then writes X and signals an event the
// replay finds reached already when it records Q's wait: the acquire is recorded all the same.
static void review3_f2_wait_met_already(void) {
    page_t g; fresh(&g);
    q_start(&g);
    cpu_write(&g, 1, 42);              // the CPU signals E before the replay records Q's wait on it
    q_acquire(&g);                     // mirror_acquire_now: the wait needs no semaphore, the acquire stays
    CHECK(g.m[1] == 42);
}

// Review 3 #3: Q writes a matrix, then a cooperative-vector conversion reads it on the CPU and
// writes its result, which Q reads after.
static void review3_f3_coopvec(void) {
    page_t g; fresh(&g);
    q_write(&g, 2, 7);
    q_release(&g);                     // before the conversion's semaphore signal
    CHECK(g.i[2] == 7);                // the CPU reads it
    cpu_write(&g, 3, 7 * 2);           // and writes the result
    q_acquire(&g);                     // after the conversion
    CHECK(g.m[3] == 14);
}

// Review 3 #4: a counter value the replay writes as it records goes into guest memory (CPU) and,
// in GPU order, into the mirror and its shadow: a later command reads it, and neither the next
// acquire nor the next publish moves it again (a later CPU write wins).
static void review3_f4_counter_report(void) {
    page_t g; fresh(&g);
    cpu_write(&g, 4, 1); g.m[4] = 1; g.s[4] = 1; // rc_counter + vka_mem_mirror_put
    CHECK(g.m[4] == 1);                // the copy after it reads 1
    q_acquire(&g); CHECK(g.m[4] == 1);
    cpu_write(&g, 4, 0);               // the game resets it after the fence
    q_write(&g, 5, 3); q_release(&g);  // a publish of another word leaves it alone
    CHECK(g.i[4] == 0);
    q_start(&g); CHECK(g.m[4] == 0);
}

// A CPU write of the value already there changes nothing; one of the value the shadow held before a
// published GPU write is copied in.
static void equal_value_cpu_write(void) {
    page_t g; fresh(&g);
    cpu_write(&g, 1, 5); q_start(&g);
    cpu_write(&g, 1, 5); q_start(&g);
    CHECK(g.m[1] == 5);
    q_write(&g, 1, 7); q_release(&g);  // published at the submission's end
    cpu_write(&g, 1, 5); q_start(&g);  // the CPU after the fence, then Q's next submission
    CHECK(g.m[1] == 5);
}

// Two NVN queues on Q's Vulkan queue: both use the mirror, in queue order; an event between them is
// met on the queue and needs no copy.
static void shared_vulkan_queue(void) {
    page_t g; fresh(&g);
    q_write(&g, 6, 1);                 // NVN queue A
    q_release(&g);                     // A signals E (a release on Q)
    CHECK(g.m[6] == 1);                // NVN queue B on Q waits E: met in queue order, reads the mirror
    q_write(&g, 6, 2);                 // B
    q_release(&g);
    CHECK(g.i[6] == 2);
}

// Bytes: Q stores byte 0 of a word and publishes; the CPU then stores byte 1 of it; Q acquires and
// stores byte 2. All stay, in the mirror and in guest memory.
static void subword_stores(void) {
    page_t g; fresh(&g);
    q_write_byte(&g, 7, 0, 0x11); q_release(&g);
    cpu_write_byte(&g, 7, 1, 0x22);
    q_start(&g);
    q_write_byte(&g, 7, 2, 0x33); q_release(&g);
    CHECK(g.m[7] == 0x332211 && g.i[7] == 0x332211);
}

int main(void) {
    review2_f2_copyin_aba();
    review2_f3_cpu_rewrites_shadow_value();
    review2_f4_one_snapshot();
    review3_f2_wait_met_already();
    review3_f3_coopvec();
    review3_f4_counter_report();
    equal_value_cpu_write();
    shared_vulkan_queue();
    subword_stores();
    return 0;
}
