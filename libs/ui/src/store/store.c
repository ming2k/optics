/* store.c — open-addressing id->node map and node lifecycle (ADR-0027). */

#include "../internal.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t slot_index(optics_ui_id id, uint32_t cap) {
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
 * OPTICS_UI_STORE_LINK_NONE (see internal.h), not 0. */

static void live_link(optics_ui_store *s, uint32_t i) {
    s->slots[i].prev_live = s->live_tail;
    if (s->live_tail != OPTICS_UI_STORE_LINK_NONE)
        s->slots[s->live_tail].next_live = i;
    else
        s->live_head = i;
    s->slots[i].next_live = OPTICS_UI_STORE_LINK_NONE;
    s->live_tail = i;
}

/* O(1) unlink; moving a probe slot repairs both neighbouring links. */
static void live_unlink(optics_ui_store *s, uint32_t i) {
    uint32_t prev = s->slots[i].prev_live, next = s->slots[i].next_live;
    if (prev != OPTICS_UI_STORE_LINK_NONE)
        s->slots[prev].next_live = next;
    else
        s->live_head = next;
    if (next != OPTICS_UI_STORE_LINK_NONE)
        s->slots[next].prev_live = prev;
    else
        s->live_tail = prev;
}

/* Backward-shift deletion preserves probe reachability without allocating.
 * cursor is the next live-list slot in the ongoing reap pass. */
static void store_erase(optics_ui_store *s, uint32_t hole, uint32_t *cursor) {
    s->epoch++;
    live_unlink(s, hole);
    s->count--;
    uint32_t mask = s->cap - 1;
    uint32_t j = (hole + 1) & mask;
    while (s->slots[j].id) {
        uint32_t home = slot_index(s->slots[j].id, s->cap);
        if (((hole - home) & mask) < ((j - home) & mask)) {
            s->slots[hole] = s->slots[j];
            uint32_t prev = s->slots[hole].prev_live, next = s->slots[hole].next_live;
            if (prev != OPTICS_UI_STORE_LINK_NONE)
                s->slots[prev].next_live = hole;
            else
                s->live_head = hole;
            if (next != OPTICS_UI_STORE_LINK_NONE)
                s->slots[next].prev_live = hole;
            else
                s->live_tail = hole;
            if (*cursor == j)
                *cursor = hole;
            hole = j;
        }
        j = (j + 1) & mask;
    }
    s->slots[hole] = (optics_ui_store_slot){};
}

static optics_gfx_result store_grow(optics_ui *ui, uint32_t new_cap) {
    optics_ui_store *s = &ui->store;
    /* Linear probing needs a free slot to terminate; refuse any capacity
     * that would leave the rehashed table at or over the 3/4 load limit. */
    if ((uint64_t)s->count * 4 >= (uint64_t)new_cap * 3)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_ui_store_slot *slots = optics_ui__alloc(ui, new_cap * sizeof *slots);
    if (!slots)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    memset(slots, 0, new_cap * sizeof *slots);

    /* Re-thread the live list in the same order as the re-inserts: walk
     * the old links (old slots keep them until freed), append each
     * relocated slot to the new tail. */
    uint32_t new_head = OPTICS_UI_STORE_LINK_NONE, new_tail = OPTICS_UI_STORE_LINK_NONE;
    for (uint32_t i = s->live_head; i != OPTICS_UI_STORE_LINK_NONE; i = s->slots[i].next_live) {
        uint32_t j = slot_index(s->slots[i].id, new_cap);
        while (slots[j].id)
            j = (j + 1) & (new_cap - 1);
        slots[j] = s->slots[i];
        slots[j].next_live = OPTICS_UI_STORE_LINK_NONE;
        slots[j].prev_live = new_tail;
        if (new_tail != OPTICS_UI_STORE_LINK_NONE)
            slots[new_tail].next_live = j;
        else
            new_head = j;
        new_tail = j;
    }
    optics_ui__free(ui, s->slots);
    s->slots = slots;
    s->cap = new_cap;
    s->epoch++;
    s->live_head = new_head;
    s->live_tail = new_tail;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_ui__store_init(optics_ui *ui, uint32_t cap) {
    if (cap > (1u << 30))
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    uint32_t c = 4;
    while (c < cap)
        c <<= 1; /* round up to power of two */
    ui->store.slots = optics_ui__alloc(ui, c * sizeof(optics_ui_store_slot));
    if (!ui->store.slots)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    memset(ui->store.slots, 0, c * sizeof(optics_ui_store_slot));
    ui->store.cap = c;
    ui->store.count = 0;
    ui->store.idle_frames = 0;
    ui->store.live_head = OPTICS_UI_STORE_LINK_NONE;
    ui->store.live_tail = OPTICS_UI_STORE_LINK_NONE;
    return OPTICS_GFX_OK;
}

void optics_ui__store_destroy(optics_ui *ui) {
    optics_ui_store *s = &ui->store;
    if (s->slots) {
        for (uint32_t i = 0; i < s->cap; i++) {
            if (!s->slots[i].id)
                continue;
            optics_ui_node *n = s->slots[i].node;
            optics_ui__node_release_cache(ui, n);
            if (n->state)
                optics_ui__free(ui, n->state);
            optics_ui__free(ui, n);
        }
        optics_ui__free(ui, s->slots);
    }
    s->slots = nullptr;
    s->cap = s->count = 0;
    s->idle_frames = 0;
    s->live_head = s->live_tail = OPTICS_UI_STORE_LINK_NONE;
}

optics_ui_node *optics_ui__store_find(const optics_ui *ui, optics_ui_id id) {
    const optics_ui_store *s = &ui->store;
    if (!s->cap || !id)
        return nullptr;
    uint32_t i = slot_index(id, s->cap);
    while (s->slots[i].id) {
        if (s->slots[i].id == id)
            return s->slots[i].node;
        i = (i + 1) & (s->cap - 1);
    }
    return nullptr;
}

static bool store_insert(optics_ui *ui, optics_ui_node *n) {
    optics_ui_store *s = &ui->store;
    /* keep load factor < 0.75 */
    if (((uint64_t)s->count + 1) * 4 >= (uint64_t)s->cap * 3) {
        if (s->cap >= (1u << 30) || store_grow(ui, s->cap * 2) != OPTICS_GFX_OK)
            return false;
    }
    uint32_t i = slot_index(n->id, s->cap);
    while (s->slots[i].id)
        i = (i + 1) & (s->cap - 1);
    s->slots[i].id = n->id;
    s->slots[i].node = n;
    s->count++;
    s->epoch++;
    live_link(s, i);
    return true;
}

optics_ui_node *optics_ui__store_touch(optics_ui *ui, optics_ui_id id) {
    optics_ui_node *n = optics_ui__store_find(ui, id);
    if (!n) {
        n = optics_ui__alloc(ui, sizeof *n);
        if (!n) {
            optics_ui__set_overflow(ui);
            return nullptr;
        }
        memset(n, 0, sizeof *n);
        n->id = id;
        n->incarnation = ++ui->next_incarnation;
        n->ui = ui;
        n->phase = OPTICS_UI_NODE_ENTERING;
        if (!store_insert(ui, n)) {
            optics_ui__free(ui, n);
            optics_ui__set_overflow(ui);
            return nullptr;
        }
    }
    if (n->last_seen != ui->frame) {
        /* A node re-entering from the LEAVING grace window must not be
         * interactive from its stale prev_rect: align with the first-frame
         * rule (no hit-testing until arranged this frame). */
        bool was_leaving = n->phase == OPTICS_UI_NODE_LEAVING;
        bool previously_seen = n->last_seen != 0;
        n->last_seen = ui->frame;
        n->leaving_frames = 0;
        if (was_leaving) {
            n->incarnation = ++ui->next_incarnation;
            n->has_prev = false;
            n->prev_rect = (optics_gfx_rect){0, 0, 0, 0};
        }
        n->phase =
            previously_seen && !was_leaving ? OPTICS_UI_NODE_STABLE : OPTICS_UI_NODE_ENTERING;
        optics_ui__node_reset_frame(n);
        /* Stamp the build-time opacity context (optics_ui_set_opacity): every
         * command pushed onto this node this frame bakes it in. */
        n->opacity = ui->opacity;
        /* Likewise the scoped frame clip. A placed node carries this stamp
         * into its band, which is what lets a chrome overlay clip absolute
         * children that deliberately escape the parent's flow viewport. */
        n->has_frame_clip = ui->clip_depth > 0;
        n->frame_clip = ui->clip;
    }
    return n;
}

/* Visit each live node once. Deletion shifts only its probe cluster and
 * cannot fail; surviving nodes are never temporarily unreachable. */
void optics_ui__store_reap(optics_ui *ui) {
    OPTICS_GFX_PROFILE_ZONE(z, "ui.store_reap");
    optics_ui_store *s = &ui->store;
    uint32_t cursor = s->live_head;
    while (cursor != OPTICS_UI_STORE_LINK_NONE) {
        uint32_t i = cursor;
        optics_ui_node *n = s->slots[i].node;
        cursor = s->slots[i].next_live;
        if (n->last_seen == ui->frame)
            continue;
        n->phase = OPTICS_UI_NODE_LEAVING;
        if (++n->leaving_frames <= OPTICS_UI_LEAVE_GRACE_FRAMES)
            continue;
        optics_ui__node_release_cache(ui, n);
        if (n->state)
            optics_ui__free(ui, n->state);
        optics_ui__free(ui, n);
        store_erase(s, i, &cursor);
    }

    /* Shrink after a sustained population collapse. A transient spike (a
     * long list or a notification burst) previously left the slot table at
     * its high-water capacity for the process lifetime — and the reap scan
     * above is O(cap) every frame, so the spike also permanently raised the
     * per-frame cost. Hysteresis: only shrink once the load has stayed
     * under 1/8 of capacity for OPTICS_UI_STORE_SHRINK_FRAMES consecutive
     * frames, and never below the initial capacity. The shrink itself is
     * the same full-rehash path as growth (amortised away by the long
     * dwell time before it can trigger again). */
    if (s->cap > OPTICS_UI_STORE_MIN_CAP && (uint64_t)s->count * 8 <= s->cap) {
        if (++s->idle_frames >= OPTICS_UI_STORE_SHRINK_FRAMES) {
            /* Entry condition (count*8 <= cap) puts cap/2 at <= 1/4 load.
             * Keep halving only while the next size stays at <= 1/4 load,
             * well clear of the 3/4 growth threshold. (The inverted test
             * this replaced halved while the table was already too small
             * and rehashed e.g. 100 live nodes into 64 slots, spinning
             * forever in store_grow's probe loop.) */
            uint32_t want = s->cap / 2;
            while (want / 2 >= OPTICS_UI_STORE_MIN_CAP && (uint64_t)s->count * 4 <= want / 2)
                want /= 2;
            /* Restart the dwell whether or not the rehash succeeded: a
             * refused or out-of-memory shrink must not be retried on every
             * following frame. */
            if (want < s->cap)
                (void)store_grow(ui, want);
            s->idle_frames = 0;
        }
    } else {
        s->idle_frames = 0;
    }

    /* Liveness reconciliation for the interaction-owned ids: a node that
     * was reaped (or never existed this run) must not stay captured,
     * focused, or scroll-hot — a stale id would otherwise retarget the next
     * widget that reuses the slot. O(1) probes against the store. */
    if (ui->active_id && !optics_ui__store_find(ui, ui->active_id))
        ui->active_id = 0;
    if (ui->scroll_hot_id && !optics_ui__store_find(ui, ui->scroll_hot_id))
        ui->scroll_hot_id = 0;
    if (ui->focused_id && !optics_ui__store_find(ui, ui->focused_id)) {
        ui->focused_id = 0;
        ui->focus_visible = false;
    }
#if OPTICS_UI_STORE_VALIDATE
    /* Debug builds re-prove the table after every frame that changed its
     * structure: a broken invariant aborts here, at the frame that broke
     * it, instead of surfacing later as a hang in a probe loop or a
     * silently lost node. Steady frames skip the O(cap) walk. */
    if (s->epoch != s->validated_epoch) {
        optics_ui__store_check(ui);
        s->validated_epoch = s->epoch;
    }
#endif
    OPTICS_GFX_PROFILE_ZONE_END(z);
}

const char *optics_ui__store_validate(const optics_ui *ui) {
    const optics_ui_store *s = &ui->store;
    if (!s->slots)
        return s->cap || s->count ? "no slot array but nonzero cap/count" : nullptr;
    if (s->cap < 4 || (s->cap & (s->cap - 1)))
        return "capacity is not a power of two >= 4";
    /* Linear probing terminates only on an empty slot, and growth keeps
     * the load under 3/4: either bound broken means a lookup or rehash
     * can spin forever. */
    if ((uint64_t)s->count * 4 >= (uint64_t)s->cap * 3)
        return "load factor at or above 3/4";
    uint32_t occupied = 0, mask = s->cap - 1;
    for (uint32_t i = 0; i < s->cap; i++) {
        optics_ui_id id = s->slots[i].id;
        if (!id) {
            if (s->slots[i].node)
                return "empty slot holds a node";
            continue;
        }
        occupied++;
        if (!s->slots[i].node || s->slots[i].node->id != id)
            return "slot id does not match its node";
        /* Reachable: no empty slot between the home slot and this one. */
        for (uint32_t j = slot_index(id, s->cap); j != i; j = (j + 1) & mask)
            if (!s->slots[j].id)
                return "occupied slot unreachable from its home (probe gap)";
        /* Unique: the first match on the probe path is this slot. */
        if (optics_ui__store_find(ui, id) != s->slots[i].node)
            return "duplicate id in the table";
    }
    if (occupied != s->count)
        return "occupied slots disagree with count";
    /* The live list threads every occupied slot exactly once. */
    uint32_t walked = 0, prev = OPTICS_UI_STORE_LINK_NONE;
    for (uint32_t i = s->live_head; i != OPTICS_UI_STORE_LINK_NONE; i = s->slots[i].next_live) {
        if (i >= s->cap || !s->slots[i].id)
            return "live list visits an empty or out-of-range slot";
        if (s->slots[i].prev_live != prev)
            return "live list back-link broken";
        if (++walked > s->count)
            return "live list longer than count (cycle or stale slot)";
        prev = i;
    }
    if (walked != s->count)
        return "live list shorter than count (lost node)";
    if (s->live_tail != prev)
        return "live list tail does not match the last node";
    return nullptr;
}

void optics_ui__store_check(const optics_ui *ui) {
    const char *broken = optics_ui__store_validate(ui);
    if (!broken)
        return;
    fprintf(stderr, "optics-ui: retained store invariant violated: %s (cap=%u count=%u)\n", broken,
            ui->store.cap, ui->store.count);
    abort();
}
