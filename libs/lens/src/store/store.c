/* store.c — open-addressing id->node map and node lifecycle (ADR-0027). */

#include "../internal.h"

#include <stdlib.h>

static uint32_t slot_index(lens_id id, uint32_t cap) {
    /* cap is a power of two; mix the id a little before masking. */
    uint64_t h = id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & (cap - 1);
}

/* Singly-linked list of live slots in insertion order, threaded through
 * a `next_live` index on each slot. The reap pass used to scan all `cap`
 * slots every frame — O(cap) even at 1 % load — so a transient
 * population spike (scrolling a 100 k-row table once) raised the
 * per-frame cost for the 300 frames the shrink hysteresis waits.
 * Maintaining the list costs one index store per insert/remove; reap
 * becomes O(live). Slot 0 is a valid index, so the terminator is
 * LENSI_STORE_LINK_NONE, not 0. */
#define LENSI_STORE_LINK_NONE UINT32_MAX

static void live_link(lens_store *s, uint32_t i) {
    if (s->live_tail != LENSI_STORE_LINK_NONE)
        s->slots[s->live_tail].next_live = i;
    else
        s->live_head = i;
    s->slots[i].next_live = LENSI_STORE_LINK_NONE;
    s->live_tail = i;
}

/* Unlink slot `i` from the live list. O(live) via head walk — called
 * only from reap's GC, which re-anchors at the head afterwards anyway
 * (a cluster clear can invalidate any saved index), so no successor
 * plumbing is needed here. */
static void live_unlink(lens_store *s, uint32_t i) {
    uint32_t prev = LENSI_STORE_LINK_NONE;
    uint32_t cur = s->live_head;
    while (cur != LENSI_STORE_LINK_NONE) {
        uint32_t next = s->slots[cur].next_live;
        if (cur == i) {
            if (prev != LENSI_STORE_LINK_NONE)
                s->slots[prev].next_live = next;
            else
                s->live_head = next;
            if (s->live_tail == i)
                s->live_tail = prev;
            return;
        }
        prev = cur;
        cur = next;
    }
}

static flux_result store_grow(lens *ui, uint32_t new_cap) {
    lens_store *s = &ui->store;
    lens_store_slot *slots = lensi_alloc(ui, new_cap * sizeof *slots);
    if (!slots)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(slots, 0, new_cap * sizeof *slots);

    /* Re-thread the live list in the same order as the re-inserts: walk
     * the old links (old slots keep them until freed), append each
     * relocated slot to the new tail. */
    uint32_t new_head = LENSI_STORE_LINK_NONE, new_tail = LENSI_STORE_LINK_NONE;
    for (uint32_t i = s->live_head; i != LENSI_STORE_LINK_NONE; i = s->slots[i].next_live) {
        uint32_t j = slot_index(s->slots[i].id, new_cap);
        while (slots[j].id)
            j = (j + 1) & (new_cap - 1);
        slots[j] = s->slots[i];
        slots[j].next_live = LENSI_STORE_LINK_NONE;
        if (new_tail != LENSI_STORE_LINK_NONE)
            slots[new_tail].next_live = j;
        else
            new_head = j;
        new_tail = j;
    }
    lensi_free(ui, s->slots);
    s->slots = slots;
    s->cap = new_cap;
    s->live_head = new_head;
    s->live_tail = new_tail;
    return FLUX_OK;
}

flux_result lensi_store_init(lens *ui, uint32_t cap) {
    uint32_t c = 1;
    while (c < cap)
        c <<= 1; /* round up to power of two */
    ui->store.slots = lensi_alloc(ui, c * sizeof(lens_store_slot));
    if (!ui->store.slots)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(ui->store.slots, 0, c * sizeof(lens_store_slot));
    ui->store.cap = c;
    ui->store.count = 0;
    ui->store.idle_frames = 0;
    ui->store.live_head = LENSI_STORE_LINK_NONE;
    ui->store.live_tail = LENSI_STORE_LINK_NONE;
    return FLUX_OK;
}

void lensi_store_destroy(lens *ui) {
    lens_store *s = &ui->store;
    if (s->slots) {
        for (uint32_t i = 0; i < s->cap; i++) {
            if (!s->slots[i].id)
                continue;
            lens_node *n = s->slots[i].node;
            lensi_node_drop_record(ui, n);
            if (n->state)
                lensi_free(ui, n->state);
            lensi_free(ui, n);
        }
        lensi_free(ui, s->slots);
    }
    s->slots = NULL;
    s->cap = s->count = 0;
    s->idle_frames = 0;
    s->live_head = s->live_tail = LENSI_STORE_LINK_NONE;
}

lens_node *lensi_store_find(const lens *ui, lens_id id) {
    const lens_store *s = &ui->store;
    if (!s->cap || !id)
        return NULL;
    uint32_t i = slot_index(id, s->cap);
    while (s->slots[i].id) {
        if (s->slots[i].id == id)
            return s->slots[i].node;
        i = (i + 1) & (s->cap - 1);
    }
    return NULL;
}

static void store_insert(lens *ui, lens_node *n) {
    lens_store *s = &ui->store;
    /* keep load factor < 0.75 */
    if ((s->count + 1) * 4 >= s->cap * 3) {
        if (store_grow(ui, s->cap * 2) != FLUX_OK)
            return;
    }
    uint32_t i = slot_index(n->id, s->cap);
    while (s->slots[i].id)
        i = (i + 1) & (s->cap - 1);
    s->slots[i].id = n->id;
    s->slots[i].node = n;
    s->count++;
    live_link(s, i);
}

lens_node *lensi_store_touch(lens *ui, lens_id id) {
    lens_node *n = lensi_store_find(ui, id);
    if (!n) {
        n = lensi_alloc(ui, sizeof *n);
        if (!n) {
            lensi_set_overflow(ui);
            return NULL;
        }
        memset(n, 0, sizeof *n);
        n->id = id;
        n->ui = ui;
        n->phase = LENS_NODE_ENTERING;
        store_insert(ui, n);
    }
    if (n->last_seen != ui->frame) {
        /* A node re-entering from the LEAVING grace window must not be
         * interactive from its stale prev_rect: align with the first-frame
         * rule (no hit-testing until arranged this frame). */
        bool was_leaving = n->phase == LENS_NODE_LEAVING;
        n->last_seen = ui->frame;
        n->leaving_frames = 0;
        if (was_leaving) {
            n->has_prev = false;
            n->prev_rect = (flux_rect){0, 0, 0, 0};
        }
        n->phase = n->has_prev ? LENS_NODE_STABLE : LENS_NODE_ENTERING;
        lensi_node_reset_frame(n);
        /* Stamp the build-time opacity context (lens_set_opacity): every
         * command pushed onto this node this frame bakes it in. */
        n->opacity = ui->opacity;
    }
    return n;
}

/* Phase advance + GC, run at lens_end after the frame is built.
 *
 * O(live) since the live-list rework: the walk follows `next_live`
 * links, so a table inflated by a transient spike (a scrolled 100
 * k-row table leaves cap at 262 144 for the 300-frame shrink
 * hysteresis) no longer costs an O(cap) scan per frame while it
 * dwells. */
/* Phase advance + GC, run at lens_end after the frame is built.
 *
 * O(live) since the live-list rework: the walk follows `next_live`
 * links, so a table inflated by a transient spike (a scrolled 100
 * k-row table leaves cap at 262 144 for the 300-frame shrink
 * hysteresis) no longer costs an O(cap) scan per frame while it
 * dwells.
 *
 * Slot removal is transactional: a reaped slot's probe cluster is
 * cleared FIRST and the displaced entries collected, THEN re-inserted.
 * store_insert may trigger store_grow (rehash), which rebuilds the
 * live list from the current links — so every unlink happens before
 * any insert can observe a half-updated list. An earlier interleaved
 * version unlinked inside the cluster walk and corrupted the list
 * exactly when a rehash fired mid-cluster. */
void lensi_store_reap(lens *ui) {
    lens_store *s = &ui->store;

    /* Displaced-by-cluster-clear entries awaiting re-insert. Grown
     * on demand; freed before the next GC victim is processed, so an
     * empty GC frame pays nothing. */
    lens_node **reinsert = NULL;
    uint32_t reinsert_cap = 0;

    uint32_t cursor = s->live_head;
    while (cursor != LENSI_STORE_LINK_NONE) {
        uint32_t i = cursor;
        lens_node *n = s->slots[i].node;

        if (n->last_seen != ui->frame) {
            n->phase = LENS_NODE_LEAVING;
            if (++n->leaving_frames > LENSI_LEAVE_GRACE_FRAMES) {
                /* Reap this node, then clear its probe cluster. The
                 * cluster clear makes intermediate live-list state
                 * visible to store_insert (which may rehash), so the
                 * order below is load-bearing: (1) unlink the victim,
                 * (2) unlink + clear EVERY displaced slot, (3) only
                 * then re-insert. The saved `cursor` is invalidated by
                 * step 2 — the walk re-anchors at the head instead. */
                live_unlink(s, i);
                lensi_node_drop_record(ui, n);
                if (n->state)
                    lensi_free(ui, n->state);
                lensi_free(ui, n);
                s->slots[i].id = 0;
                s->slots[i].node = NULL;
                s->count--;

                uint32_t moved_n = 0;
                uint32_t j = (i + 1) & (s->cap - 1);
                while (s->slots[j].id) {
                    if (moved_n == reinsert_cap) {
                        uint32_t ncap = reinsert_cap ? reinsert_cap * 2 : 16;
                        lens_node **ni = realloc(reinsert, (size_t)ncap * sizeof *ni);
                        if (!ni)
                            break; /* OOM: leave the rest of the cluster;
                                    * find() may miss entries until the
                                    * next GC pass clears them. */
                        reinsert = ni;
                        reinsert_cap = ncap;
                    }
                    reinsert[moved_n++] = s->slots[j].node;
                    live_unlink(s, j);
                    s->slots[j].id = 0;
                    s->slots[j].node = NULL;
                    s->count--;
                    j = (j + 1) & (s->cap - 1);
                }

                for (uint32_t k = 0; k < moved_n; k++)
                    store_insert(ui, reinsert[k]);
                if (reinsert) {
                    free(reinsert);
                    reinsert = NULL;
                    reinsert_cap = 0;
                }
                cursor = s->live_head; /* old index untrustworthy */
                continue;
            }
        }
        cursor = s->slots[i].next_live;
    }

    free(reinsert);

    /* Shrink after a sustained population collapse. A transient spike (a
     * long list or a notification burst) previously left the slot table at
     * its high-water capacity for the process lifetime — and the reap scan
     * above is O(cap) every frame, so the spike also permanently raised the
     * per-frame cost. Hysteresis: only shrink once the load has stayed
     * under 1/8 of capacity for LENSI_STORE_SHRINK_FRAMES consecutive
     * frames, and never below the initial capacity. The shrink itself is
     * the same full-rehash path as growth (amortised away by the long
     * dwell time before it can trigger again). */
    if (s->cap > LENSI_STORE_MIN_CAP && s->count * 8 <= s->cap) {
        if (++s->idle_frames >= LENSI_STORE_SHRINK_FRAMES) {
            uint32_t want = s->cap / 2;
            while (want > LENSI_STORE_MIN_CAP && s->count * 8 > want)
                want /= 2;
            if (want < s->cap && store_grow(ui, want) == FLUX_OK)
                s->idle_frames = 0;
        }
    } else {
        s->idle_frames = 0;
    }

    /* Liveness reconciliation for the interaction-owned ids: a node that
     * was reaped (or never existed this run) must not stay captured,
     * focused, or scroll-hot — a stale id would otherwise retarget the next
     * widget that reuses the slot. O(1) probes against the store. */
    if (ui->active_id && !lensi_store_find(ui, ui->active_id))
        ui->active_id = 0;
    if (ui->scroll_hot_id && !lensi_store_find(ui, ui->scroll_hot_id))
        ui->scroll_hot_id = 0;
    if (ui->focused_id && !lensi_store_find(ui, ui->focused_id)) {
        ui->focused_id = 0;
        ui->focus_visible = false;
    }
}
