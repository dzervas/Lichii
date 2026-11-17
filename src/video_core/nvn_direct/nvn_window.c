// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN windows in direct mode: what upstream's window block of hle_nvn.c did (left out there),
// without its pacing, frame statistics and present thread. A present hands the image to the
// replay's present queue (nvn_present.c); Lichii's hook shows it and does the present's
// bookkeeping and pacing (core/nvn_hook.cpp). The window and sync layouts are the front end's
// (nvn_obj.h).

#include <stdatomic.h>
#include <string.h>

#include "hle.h"
#include "nvn_protos.h"
#include "nvn_obj.h"
#include "nvn_capture.h"
#include "nvn_mtl.h"
#include "nvn_direct_front.h"

typedef struct { const char* name; void* fn; } nvn_impl;
// As hle_nvn.c's NVN(): an implementation with its signature checked against the prototype,
// listed in the front end's table (section swnvn).
#define NVN(name, ret, ...)                                                                        \
    static ret h_##name(__VA_ARGS__);                                                              \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                        \
    PLAT_TABLE(swnvn) static const nvn_impl e_##name = {#name, (void*)h_##name};                   \
    static ret h_##name(__VA_ARGS__)

#define WB(b) ((win_builder_t*)(b))
#define CTX(t) ((const tex_t*)(t))

static _Atomic uint64_t g_frame;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&g_frame); }

NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder* b) {
    NVNdevice* dev = WB(b)->dev;
    memset(b, 0, sizeof(win_builder_t));
    WB(b)->dev = dev;
    WB(b)->interval = 1;
}
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder* b, NVNdevice* d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder* b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder* b, int n, NVNtexture* const* t) {
    if (n < 0 || n > 4) {
        LOG("[RED] WindowBuilderSetTextures count %d", n);
        n = n < 0 ? 0 : 4;
    }
    WB(b)->ntex = n;
    for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder* b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow* w, const NVNwindowBuilder* b) {
    const win_builder_t* s = (const win_builder_t*)b;
    win_t* o = (win_t*)w;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('W', 'I', 'N', 'D');
    o->native = s->native;
    o->ntex = s->ntex;
    o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize swap chain %d textures, interval %d, first %dx%d format %#x", o->ntex, o->interval,
        o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0, o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0,
        o->ntex && o->tex[0] ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowGetPresentInterval, int, const NVNwindow* w) { return ((const win_t*)w)->interval; }
NVN(nvnWindowSetPresentInterval, void, NVNwindow* w, int v) {
    if (((win_t*)w)->interval != v) LOG("[nvn] the game sets present interval %d -> %d", ((win_t*)w)->interval, v);
    ((win_t*)w)->interval = v;
}
NVN(nvnWindowSetCrop, void, NVNwindow* w, int x, int y, int ww, int hh) {
    win_t* o = (win_t*)w;
    const int v[4] = {x, y, ww, hh};
    memcpy(o->crop, v, sizeof v);
}
// Before any SetCrop it is the whole first texture, as NVN starts it.
NVN(nvnWindowGetCrop, void, const NVNwindow* w, NVNrectangle* r) {
    const win_t* o = (const win_t*)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) {
        r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3];
        return;
    }
    r->x = r->y = 0;
    r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0;
    r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}
// The images go round in order; an acquire's sync is reached when the window's last present is:
// its queue's last submission and every fence before it (mtl_wait_acq), as on the console, where
// the image is free once the frame after it is on screen (dotfiles-25x.4). The last submission of
// any queue, as upstream's default mtl_acquire_fence, may be another queue's that waits for an
// event the game signals only after this acquire's wait (Mario Kart World's second queue: a GPU
// hang). Only the presenting queue's own fence was too early: the game then rewrote data the
// other queues still read (device lost at the title).
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow* w, NVNsync* s, int* idx) {
    win_t* o = (win_t*)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    if (s) {
        ((sync_t*)s)->fenced = 2;
        ((sync_t*)s)->fence = o->last_pfence;
    }
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue* q, NVNwindow* w, int* idx) {
    (void)q;
    return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
               ? (NVNqueueAcquireTextureResult)0
               : (NVNqueueAcquireTextureResult)1;
}
// The image goes to the replay's present queue (mtl_present: after every submission before it);
// Lichii's present on this thread then shows it (nvn_direct_present_take, core/nvn_hook.cpp).
NVN(nvnQueuePresentTexture, void, NVNqueue* q, NVNwindow* w, int idx) {
    win_t* o = (win_t*)w;
    o->presents++;
    atomic_fetch_add(&g_frame, 1);
    if (idx < 0 || idx >= o->ntex || !o->tex[idx]) return;
    o->pfence[idx] = o->last_pfence = nvn_direct_present_queue_fence = ((const queue_t*)q)->last_fence;
    const int interval = o->interval > 0 ? o->interval : 1;
    const int has_crop = o->crop[2] > 0 && o->crop[3] > 0;
    nvn_cap_present(CTX(o->tex[idx]), has_crop ? o->crop : NULL); // DM_NVN_CAPTURE
    nvn_direct_present_note(mtl_present(CTX(o->tex[idx]), interval / 60.0, has_crop ? o->crop : NULL), (uint32_t)interval);
}
