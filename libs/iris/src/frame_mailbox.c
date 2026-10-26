/* frame_mailbox.c — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Implements the wait-free atomic handoff channel between UI thread and
 * Compositor thread. See frame_mailbox.h for interface contracts. */

#include "frame_mailbox.h"
#include <string.h>

void iris_mailbox_init(iris_frame_mailbox *mb) {
    if (!mb)
        return;
    memset(mb, 0, sizeof *mb);
    mb->spare = &mb->pool[0];
    atomic_init(&mb->recycle, &mb->pool[1]);
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
    }
    mb->spare = nullptr;
    atomic_store_explicit(&mb->ready, nullptr, memory_order_relaxed);
    atomic_store_explicit(&mb->recycle, nullptr, memory_order_relaxed);
}

iris_frame_packet *iris_mailbox_acquire(iris_frame_mailbox *mb) {
    if (!mb)
        return nullptr;

    /* Check if the compositor thread returned a consumed packet. */
    iris_frame_packet *recycled =
        atomic_exchange_explicit(&mb->recycle, nullptr, memory_order_acquire);
    if (recycled) {
        if (recycled->snapshot) {
            lens_snapshot_release(recycled->snapshot);
            recycled->snapshot = nullptr;
        }
        /* If we already held a spare, stash the spare back or use recycled. */
        if (mb->spare) {
            iris_frame_packet *pick = mb->spare;
            mb->spare = recycled;
            return pick;
        }
        return recycled;
    }

    if (mb->spare) {
        iris_frame_packet *pick = mb->spare;
        mb->spare = nullptr;
        if (pick->snapshot) {
            lens_snapshot_release(pick->snapshot);
            pick->snapshot = nullptr;
        }
        return pick;
    }

    /* Fallback to pool[2] if neither spare nor recycle were populated yet */
    iris_frame_packet *pick = &mb->pool[2];
    if (pick->snapshot) {
        lens_snapshot_release(pick->snapshot);
        pick->snapshot = nullptr;
    }
    return pick;
}

void iris_mailbox_publish(iris_frame_mailbox *mb, iris_frame_packet *pkt) {
    if (!mb || !pkt)
        return;

    pkt->sequence = atomic_fetch_add_explicit(&mb->sequence, 1, memory_order_relaxed) + 1;
    if (pkt->snapshot)
        lens_snapshot_retain(pkt->snapshot);

    /* Atomic handoff to the compositor thread. */
    iris_frame_packet *superseded = atomic_exchange_explicit(&mb->ready, pkt, memory_order_release);
    if (superseded) {
        /* A previously published packet was not consumed by the compositor before
         * this newer packet arrived. Release its snapshot and retain it as our spare. */
        if (superseded->snapshot) {
            lens_snapshot_release(superseded->snapshot);
            superseded->snapshot = nullptr;
        }
        if (mb->spare && mb->spare != superseded && mb->spare->snapshot) {
            lens_snapshot_release(mb->spare->snapshot);
            mb->spare->snapshot = nullptr;
        }
        mb->spare = superseded;
    }
}

iris_frame_packet *iris_mailbox_claim(iris_frame_mailbox *mb) {
    if (!mb)
        return nullptr;
    return atomic_exchange_explicit(&mb->ready, nullptr, memory_order_acquire);
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
    }
}
