/*
 * damage_history.c — write-epoch history for swapchain rotation repair
 * (ADR-0149). See damage_history.h for the contract.
 */

#include "damage_history.h"

#include <string.h>

#define DISJOINT_WORKLIST_CAP (FLUX_DAMAGE_HISTORY_RECTS * 4u)

/* Insert `r` (which must have positive width and height) into `out[0..*count)`
 * by subtracting every existing rectangle from `r` so the resulting array is
 * strictly pairwise-disjoint. Returns false if the disjoint decomposition
 * cannot fit in `capacity` (or the internal worklist), in which case the caller
 * must degrade to a full-extent copy. */
bool flux_rect_set_append_disjoint(flux_rect *out, uint32_t *count, uint32_t capacity,
                                   flux_rect r) {
    if (!out || capacity == 0)
        return false;

    flux_rect pending[DISJOINT_WORKLIST_CAP];
    flux_rect next[DISJOINT_WORKLIST_CAP];
    pending[0] = r;
    uint32_t pending_count = 1;

    const uint32_t existing_count = *count;
    for (uint32_t i = 0; i < existing_count && pending_count > 0; i++) {
        const flux_rect e = out[i];
        const float ex0 = e.x;
        const float ey0 = e.y;
        const float ex1 = e.x + e.w;
        const float ey1 = e.y + e.h;

        uint32_t next_count = 0;
        for (uint32_t p = 0; p < pending_count; p++) {
            const flux_rect c = pending[p];
            const float cx0 = c.x;
            const float cy0 = c.y;
            const float cx1 = c.x + c.w;
            const float cy1 = c.y + c.h;

            const float ix0 = cx0 > ex0 ? cx0 : ex0;
            const float iy0 = cy0 > ey0 ? cy0 : ey0;
            const float ix1 = cx1 < ex1 ? cx1 : ex1;
            const float iy1 = cy1 < ey1 ? cy1 : ey1;

            if (!(ix0 < ix1 && iy0 < iy1)) {
                if (next_count >= DISJOINT_WORKLIST_CAP)
                    return false;
                next[next_count++] = c;
                continue;
            }

            if (cy0 < iy0) {
                if (next_count >= DISJOINT_WORKLIST_CAP)
                    return false;
                next[next_count++] = (flux_rect){cx0, cy0, cx1 - cx0, iy0 - cy0};
            }
            if (iy1 < cy1) {
                if (next_count >= DISJOINT_WORKLIST_CAP)
                    return false;
                next[next_count++] = (flux_rect){cx0, iy1, cx1 - cx0, cy1 - iy1};
            }
            if (cx0 < ix0) {
                if (next_count >= DISJOINT_WORKLIST_CAP)
                    return false;
                next[next_count++] = (flux_rect){cx0, iy0, ix0 - cx0, iy1 - iy0};
            }
            if (ix1 < cx1) {
                if (next_count >= DISJOINT_WORKLIST_CAP)
                    return false;
                next[next_count++] = (flux_rect){ix1, iy0, cx1 - ix1, iy1 - iy0};
            }
        }
        memcpy(pending, next, next_count * sizeof(flux_rect));
        pending_count = next_count;
    }

    if (pending_count == 0)
        return true;
    if (*count + pending_count > capacity)
        return false;
    memcpy(&out[*count], pending, pending_count * sizeof(flux_rect));
    *count += pending_count;
    return true;
}

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

    /* `full` is the explicit "whole extent" verdict. Empty + full=false is a
     * real no-change epoch: a frame can be presented for pacing or to repair
     * history without writing a new pixel. Treating that as full made every
     * windowed content sync copy the whole swapchain image on every frame. */
    uint32_t n = 0;
    if (!full && rects && count > 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (!(rects[i].w > 0.0f) || !(rects[i].h > 0.0f))
                continue; /* names no pixel; not a reason to widen the frame */
            if (!flux_rect_set_append_disjoint(slot->rects, &n, FLUX_DAMAGE_HISTORY_RECTS,
                                               rects[i])) {
                n = 0;
                full = true;
                break;
            }
        }
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
        for (uint32_t r = 0; r < e->count; r++) {
            /* Decompose across epochs so `vkCmdCopyImage` never receives
             * overlapping regions (VUID-vkCmdCopyImage-pRegions-00124). An
             * entry whose disjoint split cannot fit the caller's buffer falls
             * back to a full copy rather than under-covering. */
            if (!flux_rect_set_append_disjoint(out, &n, capacity, e->rects[r]))
                return 0;
        }
    }
    if (out_full)
        *out_full = false;
    return n;
}
