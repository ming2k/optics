/* frame_mailbox.c — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Implements the wait-free atomic handoff channel between UI thread and
 * Compositor thread. See frame_mailbox.h for interface contracts. */

#include "frame_mailbox.h"
#include <flux/profile.h>
#include <math.h>
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
static iris_frame_damage iris_damage_from(const flux_rect *rects, uint32_t count, float scale,
                                          int32_t width, int32_t height, bool force_full) {
    iris_frame_damage out = {0};
    if (force_full || !rects || count == 0 || width <= 0 || height <= 0) {
        out.full = true;
        return out;
    }
    const float s = scale > 0.0f ? scale : 1.0f;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count && kept < LENS_DAMAGE_RECTS_CAPACITY; i++) {
        const flux_rect d = rects[i];
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
        out.rects[kept++] = (flux_rect){x0, y0, x1 - x0, y1 - y0};
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

iris_frame_damage iris_frame_packet_damage(const iris_frame_packet *pkt) {
    iris_frame_damage out = {0};
    if (!pkt) {
        out.full = true;
        return out;
    }
    const uint32_t count = pkt->damage_count < LENS_DAMAGE_RECTS_CAPACITY
                               ? pkt->damage_count
                               : LENS_DAMAGE_RECTS_CAPACITY;
    /* The packet carries logical extents plus the scale; the rects are logical
     * too. Deriving the device extent here (rather than trusting a caller to
     * pass one convention) is what stops the clip from being applied in the
     * wrong units — the two callers previously disagreed about it. */
    const float s = pkt->scale > 0.0f ? pkt->scale : 1.0f;
    const int32_t dev_w = (int32_t)ceilf((float)pkt->width * s);
    const int32_t dev_h = (int32_t)ceilf((float)pkt->height * s);
    return iris_damage_from(pkt->damage, count, s, dev_w, dev_h, pkt->force_paint);
}

iris_frame_damage iris_frame_lens_damage(const lens *ui, float scale, int32_t width, int32_t height,
                                         bool force_full) {
    flux_rect rects[LENS_DAMAGE_RECTS_CAPACITY];
    const uint32_t count = lens_damage_rects(
        ui, &(lens_damage_opts){.rects = rects, .capacity = LENS_DAMAGE_RECTS_CAPACITY});
    return iris_damage_from(rects, count, scale, width, height, force_full);
}

void iris_mailbox_init(iris_frame_mailbox *mb) {
    if (!mb)
        return;
    memset(mb, 0, sizeof *mb);
    mb->spare = &mb->pool[0];
    mb->spare2 = &mb->pool[1];
    atomic_init(&mb->recycle, &mb->pool[2]);
    atomic_init(&mb->ready, nullptr);
    atomic_init(&mb->sequence, 0);
}

void iris_mailbox_destroy(iris_frame_mailbox *mb) {
    if (!mb)
        return;
    for (int i = 0; i < 3; i++) {
        if (mb->pool[i].snapshot) {
            lens_snapshot_release(mb->pool[i].snapshot);
            mb->pool[i].snapshot = nullptr;
        }
        if (mb->pool[i].host_list) {
            flux_display_list_release(mb->pool[i].host_list);
            mb->pool[i].host_list = nullptr;
        }
    }
    mb->spare = nullptr;
    mb->spare2 = nullptr;
    atomic_store_explicit(&mb->ready, nullptr, memory_order_relaxed);
    atomic_store_explicit(&mb->recycle, nullptr, memory_order_relaxed);
}

iris_frame_packet *iris_mailbox_acquire(iris_frame_mailbox *mb) {
    if (!mb)
        return nullptr;

    while (true) {
        /* Check if the compositor thread returned a consumed packet. */
        iris_frame_packet *recycled =
            atomic_exchange_explicit(&mb->recycle, nullptr, memory_order_acquire);
        if (recycled) {
            if (recycled->snapshot) {
                lens_snapshot_release(recycled->snapshot);
                recycled->snapshot = nullptr;
            }
            if (recycled->host_list) {
                flux_display_list_release(recycled->host_list);
                recycled->host_list = nullptr;
            }
            if (!mb->spare) {
                mb->spare = recycled;
            } else if (!mb->spare2) {
                mb->spare2 = recycled;
            } else {
                return recycled;
            }
        }

        if (mb->spare) {
            iris_frame_packet *pick = mb->spare;
            mb->spare = nullptr;
            if (pick->snapshot) {
                lens_snapshot_release(pick->snapshot);
                pick->snapshot = nullptr;
            }
            if (pick->host_list) {
                flux_display_list_release(pick->host_list);
                pick->host_list = nullptr;
            }
            return pick;
        }

        if (mb->spare2) {
            iris_frame_packet *pick = mb->spare2;
            mb->spare2 = nullptr;
            if (pick->snapshot) {
                lens_snapshot_release(pick->snapshot);
                pick->snapshot = nullptr;
            }
            if (pick->host_list) {
                flux_display_list_release(pick->host_list);
                pick->host_list = nullptr;
            }
            return pick;
        }

        /* All 3 buffers in flight: yield momentarily to let compositor recycle. */
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}

void iris_mailbox_publish(iris_frame_mailbox *mb, iris_frame_packet *pkt) {
    if (!mb || !pkt)
        return;

    FLUX_PROFILE_ZONE(z, "iris.mailbox_publish");
    pkt->sequence = atomic_fetch_add_explicit(&mb->sequence, 1, memory_order_relaxed) + 1;
    FLUX_PROFILE_PLOT_INT("iris.mailbox_seq", (int64_t)pkt->sequence);
    if (pkt->snapshot)
        lens_snapshot_retain(pkt->snapshot);
    if (pkt->host_list)
        flux_display_list_retain(pkt->host_list);

    /* Atomic handoff to the compositor thread. */
    iris_frame_packet *superseded = atomic_exchange_explicit(&mb->ready, pkt, memory_order_release);
    if (superseded) {
        if (superseded->snapshot) {
            lens_snapshot_release(superseded->snapshot);
            superseded->snapshot = nullptr;
        }
        if (superseded->host_list) {
            flux_display_list_release(superseded->host_list);
            superseded->host_list = nullptr;
        }
        if (!mb->spare) {
            mb->spare = superseded;
        } else if (!mb->spare2) {
            mb->spare2 = superseded;
        }
    }
    FLUX_PROFILE_ZONE_END(z);
}

iris_frame_packet *iris_mailbox_claim(iris_frame_mailbox *mb) {
    if (!mb)
        return nullptr;
    FLUX_PROFILE_ZONE(z, "iris.mailbox_claim");
    iris_frame_packet *pkt = atomic_exchange_explicit(&mb->ready, nullptr, memory_order_acquire);
    FLUX_PROFILE_ZONE_END(z);
    return pkt;
}

void iris_mailbox_recycle(iris_frame_mailbox *mb, iris_frame_packet *pkt) {
    if (!mb || !pkt)
        return;

    /* Hand the consumed packet back to the UI thread. */
    iris_frame_packet *stale = atomic_exchange_explicit(&mb->recycle, pkt, memory_order_release);
    if (stale) {
        if (stale->snapshot) {
            lens_snapshot_release(stale->snapshot);
            stale->snapshot = nullptr;
        }
        if (stale->host_list) {
            flux_display_list_release(stale->host_list);
            stale->host_list = nullptr;
        }
    }
}
