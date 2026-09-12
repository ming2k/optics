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
    s->slots[i].prev_live = s->live_tail;
    if (s->live_tail != LENSI_STORE_LINK_NONE)
        s->slots[s->live_tail].next_live = i;
    else
        s->live_head = i;
    s->slots[i].next_live = LENSI_STORE_LINK_NONE;
    s->live_tail = i;
}

/* O(1) unlink; moving a probe slot repairs both neighbouring links. */
static void live_unlink(lens_store *s, uint32_t i) {
    uint32_t prev = s->slots[i].prev_live, next = s->slots[i].next_live;
    if (prev != LENSI_STORE_LINK_NONE)
        s->slots[prev].next_live = next;
    else
        s->live_head = next;
    if (next != LENSI_STORE_LINK_NONE)
        s->slots[next].prev_live = prev;
    else
        s->live_tail = prev;
}

/* Backward-shift deletion preserves probe reachability without allocating.
 * cursor is the next live-list slot in the ongoing reap pass. */
static void store_erase(lens_store *s, uint32_t hole, uint32_t *cursor) {
    live_unlink(s, hole);
    s->count--;
    uint32_t mask = s->cap - 1;
    uint32_t j = (hole + 1) & mask;
    while (s->slots[j].id) {
        uint32_t home = slot_index(s->slots[j].id, s->cap);
        if (((hole - home) & mask) < ((j - home) & mask)) {
            s->slots[hole] = s->slots[j];
            uint32_t prev = s->slots[hole].prev_live, next = s->slots[hole].next_live;
            if (prev != LENSI_STORE_LINK_NONE)
                s->slots[prev].next_live = hole;
            else
                s->live_head = hole;
            if (next != LENSI_STORE_LINK_NONE)
                s->slots[next].prev_live = hole;
            else
                s->live_tail = hole;
            if (*cursor == j)
                *cursor = hole;
            hole = j;
        }
        j = (j + 1) & mask;
    }
    s->slots[hole] = (lens_store_slot){};
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
        slots[j].prev_live = new_tail;
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
    if (cap > (1u << 30))
        return FLUX_ERROR_INVALID_ARGUMENT;
    uint32_t c = 4;
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

static bool store_insert(lens *ui, lens_node *n) {
    lens_store *s = &ui->store;
    /* keep load factor < 0.75 */
    if (((uint64_t)s->count + 1) * 4 >= (uint64_t)s->cap * 3) {
        if (s->cap >= (1u << 30) || store_grow(ui, s->cap * 2) != FLUX_OK)
            return false;
    }
    uint32_t i = slot_index(n->id, s->cap);
    while (s->slots[i].id)
        i = (i + 1) & (s->cap - 1);
    s->slots[i].id = n->id;
    s->slots[i].node = n;
    s->count++;
    live_link(s, i);
    return true;
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
        if (!store_insert(ui, n)) {
            lensi_free(ui, n);
            lensi_set_overflow(ui);
            return NULL;
        }
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

/* Visit each live node once. Deletion shifts only its probe cluster and
 * cannot fail; surviving nodes are never temporarily unreachable. */
void lensi_store_reap(lens *ui) {
    lens_store *s = &ui->store;
    uint32_t cursor = s->live_head;
    while (cursor != LENSI_STORE_LINK_NONE) {
        uint32_t i = cursor;
        lens_node *n = s->slots[i].node;
        cursor = s->slots[i].next_live;
        if (n->last_seen == ui->frame)
            continue;
        n->phase = LENS_NODE_LEAVING;
        if (++n->leaving_frames <= LENSI_LEAVE_GRACE_FRAMES)
            continue;
        lensi_node_drop_record(ui, n);
        if (n->state)
            lensi_free(ui, n->state);
        lensi_free(ui, n);
        store_erase(s, i, &cursor);
    }

    /* Shrink after a sustained population collapse. A transient spike (a
     * long list or a notification burst) previously left the slot table at
     * its high-water capacity for the process lifetime — and the reap scan
     * above is O(cap) every frame, so the spike also permanently raised the
     * per-frame cost. Hysteresis: only shrink once the load has stayed
     * under 1/8 of capacity for LENSI_STORE_SHRINK_FRAMES consecutive
     * frames, and never below the initial capacity. The shrink itself is
     * the same full-rehash path as growth (amortised away by the long
     * dwell time before it can trigger again). */
    if (s->cap > LENSI_STORE_MIN_CAP && (uint64_t)s->count * 8 <= s->cap) {
        if (++s->idle_frames >= LENSI_STORE_SHRINK_FRAMES) {
            uint32_t want = s->cap / 2;
            while (want > LENSI_STORE_MIN_CAP && (uint64_t)s->count * 8 > want)
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
