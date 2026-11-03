/* frame_mailbox.c — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Implements the wait-free atomic handoff channel between UI thread and
 * Compositor thread. See frame_mailbox.h for interface contracts. */

#include "frame_mailbox.h"
#include <flux/profile.h>
#include <math.h>
#include <string.h>

/* The one place a damage set becomes a single device-pixel rectangle, for the
 * backends whose presentation API accepts exactly one.
 *
 * `rects` are UI-logical; the result is device pixels, clipped to the surface
 * because a backend rejects an out-of-bounds area outright and losing the pass
 * loses the frame's content, not just its scissor. Returns false when the set
 * covers nothing, which the caller must treat as "paint everything". */
static bool iris_damage_rect_from(const flux_rect *rects, uint32_t count, float scale,
                                  int32_t width, int32_t height, flux_rect *out) {
    if (out)
        *out = (flux_rect){0, 0, 0, 0};
    if (!out || !rects || count == 0 || width <= 0 || height <= 0)
        return false;
    const float s = scale > 0.0f ? scale : 1.0f;
    flux_rect covered = {0, 0, 0, 0};
    for (uint32_t i = 0; i < count; i++) {
        const flux_rect d = rects[i];
        const float x0 = floorf(d.x * s);
        const float y0 = floorf(d.y * s);
        const float x1 = ceilf((d.x + d.w) * s);
        const float y1 = ceilf((d.y + d.h) * s);
        covered = flux_rect_union(covered, (flux_rect){x0, y0, x1 - x0, y1 - y0});
    }
    if (covered.w <= 0.0f || covered.h <= 0.0f)
        return false;
    const float x0 = covered.x < 0.0f ? 0.0f : covered.x;
    const float y0 = covered.y < 0.0f ? 0.0f : covered.y;
    float x1 = covered.x + covered.w;
    float y1 = covered.y + covered.h;
    if (x1 > (float)width)
        x1 = (float)width;
    if (y1 > (float)height)
        y1 = (float)height;
    if (x1 <= x0 || y1 <= y0)
        return false;
    *out = (flux_rect){x0, y0, x1 - x0, y1 - y0};
    return true;
}

bool iris_frame_packet_damage_rect(const iris_frame_packet *pkt, float scale, int32_t width,
                                   int32_t height, flux_rect *out) {
    if (!pkt || pkt->damage_count == 0) {
        if (out)
            *out = (flux_rect){0, 0, 0, 0};
        return false;
    }
    const uint32_t count = pkt->damage_count < LENS_DAMAGE_RECTS_CAPACITY
                               ? pkt->damage_count
                               : LENS_DAMAGE_RECTS_CAPACITY;
    return iris_damage_rect_from(pkt->damage, count, scale, width, height, out);
}

bool iris_frame_lens_damage_rect(const lens *ui, float scale, int32_t width, int32_t height,
                                 flux_rect *out) {
    flux_rect rects[LENS_DAMAGE_RECTS_CAPACITY];
    const uint32_t count = lens_damage_rects(
        ui, &(lens_damage_opts){.rects = rects, .capacity = LENS_DAMAGE_RECTS_CAPACITY});
    return iris_damage_rect_from(rects, count, scale, width, height, out);
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
