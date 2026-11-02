/* frame_mailbox.c — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Implements the wait-free atomic handoff channel between UI thread and
 * Compositor thread. See frame_mailbox.h for interface contracts. */

#include "frame_mailbox.h"
#include <flux/profile.h>
#include <string.h>

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
