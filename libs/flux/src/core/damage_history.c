/*
 * damage_history.c — write-epoch history for swapchain rotation repair
 * (ADR-0149). See damage_history.h for the contract.
 */

#include "damage_history.h"

#include <string.h>

void flux_damage_history_reset(flux_damage_history *h) {
    if (!h)
        return;
    memset(h, 0, sizeof *h);
}

uint64_t flux_damage_history_commit(flux_damage_history *h, const flux_rect *rects, uint32_t count,
                                    bool full) {
    if (!h)
        return 0;
    const uint64_t epoch = h->committed + 1;
    flux_damage_epoch *slot = &h->entries[epoch % FLUX_DAMAGE_HISTORY_DEPTH];
    slot->epoch = epoch;

    /* An empty set means the frame could not localise itself: treat it as the
     * whole extent rather than as "nothing changed". A frame that genuinely wrote
     * nothing should not be committing at all, and if it does, over-copying costs
     * a blit while under-copying leaves stale pixels on screen. */
    uint32_t n = 0;
    uint32_t live = 0;
    if (!full && rects && count > 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (!(rects[i].w > 0.0f) || !(rects[i].h > 0.0f))
                continue; /* names no pixel; not a reason to widen the frame */
            live++;
            if (n < FLUX_DAMAGE_HISTORY_RECTS)
                slot->rects[n++] = rects[i];
        }
        /* More disjoint regions than the slot can name: the omitted ones have no
         * coordinates to contribute, so the whole extent is the only answer that
         * covers them. A degenerate rect that was merely *dropped* above is not
         * this case — it covers nothing. */
        if (live > FLUX_DAMAGE_HISTORY_RECTS) {
            n = 0;
            full = true;
        }
        if (n == 0 && !full)
            full = true;
    } else {
        full = true;
    }
    slot->count = n;
    slot->full = full;
    h->committed = epoch;
    return epoch;
}

uint32_t flux_damage_history_since(const flux_damage_history *h, uint64_t since, flux_rect *out,
                                   uint32_t capacity, bool *out_full) {
    if (out_full)
        *out_full = true;
    if (!h)
        return 0;

    /* Never synchronised: the image holds nothing this surface would reproduce. */
    if (since == FLUX_DAMAGE_EPOCH_NONE)
        return 0;

    /* No history at all — nothing has been committed, or a reset just discarded
     * everything. Any `since` then refers to an epoch this surface no longer has
     * any record of, which is the same fact as "never synchronised". */
    if (h->committed == 0)
        return 0;

    /* Already current: no copy at all. This is the steady-state common case. */
    if (since >= h->committed) {
        if (out_full)
            *out_full = false;
        return 0;
    }

    /* The retained window is `(committed - DEPTH, committed]`. An entry the image
     * predates has already been overwritten, so the frames it missed cannot be
     * reconstructed and it takes a full copy. Compare with a subtraction that
     * cannot underflow: `since < committed` here, so the difference is safe. */
    if (h->committed - since > FLUX_DAMAGE_HISTORY_DEPTH)
        return 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < FLUX_DAMAGE_HISTORY_DEPTH; i++) {
        const flux_damage_epoch *e = &h->entries[i];
        if (e->epoch == 0 || e->epoch <= since)
            continue;
        if (e->full)
            return 0;
        /* Count before copying: an entry that cannot fit the caller's buffer is
         * an unknown set, and a partial list would under-cover. */
        if (!out || n + e->count > capacity)
            return 0;
        for (uint32_t r = 0; r < e->count; r++)
            out[n++] = e->rects[r];
    }
    if (out_full)
        *out_full = false;
    return n;
}
