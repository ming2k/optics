/* store.c — open-addressing id->node map and node lifecycle (ADR-0027). */

#include "../internal.h"

static uint32_t slot_index(lens_id id, uint32_t cap) {
    /* cap is a power of two; mix the id a little before masking. */
    uint64_t h = id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & (cap - 1);
}

static flux_result store_grow(lens *ui, uint32_t new_cap) {
    lens_store *s = &ui->store;
    lens_store_slot *slots = lensi_alloc(ui, new_cap * sizeof *slots);
    if (!slots)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(slots, 0, new_cap * sizeof *slots);

    for (uint32_t i = 0; i < s->cap; i++) {
        if (!s->slots[i].id)
            continue;
        uint32_t j = slot_index(s->slots[i].id, new_cap);
        while (slots[j].id)
            j = (j + 1) & (new_cap - 1);
        slots[j] = s->slots[i];
    }
    lensi_free(ui, s->slots);
    s->slots = slots;
    s->cap = new_cap;
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
        n->last_seen = ui->frame;
        n->leaving_frames = 0;
        n->phase = n->has_prev ? LENS_NODE_STABLE : LENS_NODE_ENTERING;
        lensi_node_reset_frame(n);
    }
    return n;
}

/* Phase advance + GC, run at lens_end after the frame is built. */
void lensi_store_reap(lens *ui) {
    lens_store *s = &ui->store;
    for (uint32_t i = 0; i < s->cap; i++) {
        if (!s->slots[i].id)
            continue;
        lens_node *n = s->slots[i].node;
        if (n->last_seen == ui->frame)
            continue; /* alive */

        n->phase = LENS_NODE_LEAVING;
        if (++n->leaving_frames <= LENSI_LEAVE_GRACE_FRAMES)
            continue;

        /* reap: free node, tombstone-free by full rehash of the run */
        lensi_node_drop_record(ui, n);
        if (n->state)
            lensi_free(ui, n->state);
        lensi_free(ui, n);
        s->slots[i].id = 0;
        s->slots[i].node = NULL;
        s->count--;

        /* re-insert the following cluster to preserve open-addressing */
        uint32_t j = (i + 1) & (s->cap - 1);
        while (s->slots[j].id) {
            lens_store_slot moved = s->slots[j];
            s->slots[j].id = 0;
            s->slots[j].node = NULL;
            s->count--;
            store_insert(ui, moved.node);
            j = (j + 1) & (s->cap - 1);
        }
    }
}
