/* frame_mailbox.c — wait-free triple-buffered snapshot exchange (ADR-0124).
 *
 * Implements the wait-free atomic handoff channel between UI thread and
 * Compositor thread. See frame_mailbox.h for interface contracts. */

#include "frame_mailbox.h"
#include "ui_host.h"
#include <math.h>
#include <optics/gfx/profile.h>
#include <string.h>

/* Convert a UI-logical damage set into device pixels, clipped to the surface.
 *
 * Every rectangle survives: this is the boundary where units change, not where
 * a set becomes a box. A rectangle that scales to nothing, or that lies entirely
 * outside the surface, is dropped — an out-of-bounds area makes a backend reject
 * the whole pass, which loses the frame's content rather than just its scissor.
 *
 * `full` is reported whenever the answer cannot be a list: the caller asked for
 * everything, the set was empty, or nothing survived the clip. "Empty" and
 * "everything" are different facts and are never conflated here. */
static optics_window_frame_damage optics_window_damage_from(const optics_gfx_rect *rects,
                                                            uint32_t count, float scale,
                                                            int32_t width, int32_t height,
                                                            bool force_full) {
    optics_window_frame_damage out = {};
    if (force_full || !rects || count == 0 || width <= 0 || height <= 0) {
        out.full = true;
        return out;
    }
    const float s = scale > 0.0f ? scale : 1.0f;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count && kept < OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY; i++) {
        const optics_gfx_rect d = rects[i];
        if (!(d.w > 0.0f) || !(d.h > 0.0f))
            continue;
        float x0 = floorf(d.x * s);
        float y0 = floorf(d.y * s);
        float x1 = ceilf((d.x + d.w) * s);
        float y1 = ceilf((d.y + d.h) * s);
        if (x0 < 0.0f)
            x0 = 0.0f;
        if (y0 < 0.0f)
            y0 = 0.0f;
        if (x1 > (float)width)
            x1 = (float)width;
        if (y1 > (float)height)
            y1 = (float)height;
        if (x1 <= x0 || y1 <= y0)
            continue;
        out.rects[kept++] = (optics_gfx_rect){x0, y0, x1 - x0, y1 - y0};
    }
    if (kept == 0) {
        /* Every named region fell outside the surface. The frame still needs to
         * paint — a caller that passes a set covering nothing observable is
         * asking for a repaint, not for a no-op. */
        out.full = true;
        return out;
    }
    out.count = kept;
    return out;
}

optics_window_frame_damage
optics_window_frame_packet_damage(const optics_window_frame_packet *pkt) {
    optics_window_frame_damage out = {};
    if (!pkt) {
        out.full = true;
        return out;
    }
    const uint32_t count = pkt->damage_count < OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY
                               ? pkt->damage_count
                               : OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY;
    /* The packet carries logical extents plus the scale; the rects are logical
     * too. Deriving the device extent here (rather than trusting a caller to
     * pass one convention) is what stops the clip from being applied in the
     * wrong units — the two callers previously disagreed about it. */
    const float s = pkt->scale > 0.0f ? pkt->scale : 1.0f;
    const int32_t dev_w = (int32_t)ceilf((float)pkt->width * s);
    const int32_t dev_h = (int32_t)ceilf((float)pkt->height * s);
    return optics_window_damage_from(pkt->damage, count, s, dev_w, dev_h, pkt->force_paint);
}

optics_window_frame_damage optics_window_frame_ui_damage(const optics_window_ui_host *ui,
                                                         float scale, int32_t width, int32_t height,
                                                         bool force_full) {
    optics_gfx_rect rects[OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY];
    const uint32_t count =
        optics_window__ui_damage_rects(ui, rects, OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY);
    return optics_window_damage_from(rects, count, scale, width, height, force_full);
}

#define OPTICS_WINDOW_MAILBOX_FRESH 0x80000000u
#define OPTICS_WINDOW_MAILBOX_INDEX 0x3u

/* Drop the references a packet holds. Runs on whichever thread owns the
 * packet at the time; in practice the UI thread (acquire) or teardown. */
static void packet_clear(optics_window_frame_mailbox *mb, optics_window_frame_packet *pkt) {
    optics_window__ui_snapshot_release(mb->ui, pkt->snapshot);
    pkt->snapshot = nullptr;
    if (pkt->host_list)
        optics_gfx_display_list_release(pkt->host_list);
    pkt->host_list = nullptr;
}

void optics_window_mailbox_init(optics_window_frame_mailbox *mb, const optics_window_ui_host *ui) {
    if (!mb)
        return;
    memset(mb, 0, sizeof *mb);
    mb->ui = ui;
    mb->back = 0;
    mb->front = 2;
    atomic_init(&mb->middle, 1u);
    atomic_init(&mb->sequence, 0);
}

void optics_window_mailbox_destroy(optics_window_frame_mailbox *mb) {
    if (!mb)
        return;
    for (int i = 0; i < 3; i++)
        packet_clear(mb, &mb->pool[i]);
}

optics_window_frame_packet *optics_window_mailbox_acquire(optics_window_frame_mailbox *mb) {
    if (!mb)
        return nullptr;
    optics_window_frame_packet *pkt = &mb->pool[mb->back];
    packet_clear(mb, pkt);
    return pkt;
}

void optics_window_mailbox_publish(optics_window_frame_mailbox *mb,
                                   optics_window_frame_packet *pkt) {
    if (!mb || pkt != &mb->pool[mb->back])
        return;

    OPTICS_GFX_PROFILE_ZONE(z, "window.mailbox_publish");
    pkt->sequence = atomic_fetch_add_explicit(&mb->sequence, 1, memory_order_relaxed) + 1;
    OPTICS_GFX_PROFILE_PLOT_INT("window.mailbox_seq", (int64_t)pkt->sequence);
    optics_window__ui_snapshot_retain(mb->ui, pkt->snapshot);
    if (pkt->host_list)
        optics_gfx_display_list_retain(pkt->host_list);

    /* Hand the filled packet over and take whatever was in the shared slot:
     * a spare, or a frame the presenter never got to (superseded). */
    const uint32_t old = atomic_exchange_explicit(
        &mb->middle, mb->back | OPTICS_WINDOW_MAILBOX_FRESH, memory_order_acq_rel);
    mb->back = old & OPTICS_WINDOW_MAILBOX_INDEX;
    OPTICS_GFX_PROFILE_ZONE_END(z);
}

optics_window_frame_packet *optics_window_mailbox_claim(optics_window_frame_mailbox *mb) {
    if (!mb)
        return nullptr;
    /* Only this thread ever clears FRESH, so once it is observed set the
     * exchange below is guaranteed to take a fresh packet — possibly a newer
     * one than the load saw, which is exactly what a claim should return. */
    if (!(atomic_load_explicit(&mb->middle, memory_order_acquire) & OPTICS_WINDOW_MAILBOX_FRESH))
        return nullptr;
    OPTICS_GFX_PROFILE_ZONE(z, "window.mailbox_claim");
    const uint32_t old = atomic_exchange_explicit(&mb->middle, mb->front, memory_order_acq_rel);
    mb->front = old & OPTICS_WINDOW_MAILBOX_INDEX;
    OPTICS_GFX_PROFILE_ZONE_END(z);
    return &mb->pool[mb->front];
}

bool optics_window_mailbox_pending(const optics_window_frame_mailbox *mb) {
    return mb &&
           (atomic_load_explicit(&mb->middle, memory_order_acquire) & OPTICS_WINDOW_MAILBOX_FRESH);
}
