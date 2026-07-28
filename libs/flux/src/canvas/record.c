/*
 * Canvas display-list segments — record / replay (see internal.h
 * "Display-list segments" for the design contract).
 *
 * Recording is a passive front-end layer over the backend vtable:
 * canvas_emit captures every batch (pipeline id, push block, scissor,
 * blend, vertex bytes) into each active recording before submitting it
 * live. Replay re-validates the recorded anchor (framebuffer extent,
 * absolute transform, incoming scissor) and re-submits the captured
 * batches through the same backend entry points, so no backend needs to
 * know segments exist.
 */
#include "backend.h"
#include "internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Slot pool                                                         */
/* ------------------------------------------------------------------ */

static void slot_free_contents(flux_canvas *c, flux_canvas_record_slot *s) {
    for (uint32_t i = 0; i < s->image_count; ++i)
        flux_image_release(s->images[i]);
    s->image_count = 0;
    for (uint32_t i = 0; i < s->sampler_count; ++i)
        flux_sampler_release(s->samplers[i]);
    s->sampler_count = 0;
    flux_canvas_free(c->device, s->ops);
    flux_canvas_free(c->device, s->verts);
    s->ops = nullptr;
    s->verts = nullptr;
    s->op_count = s->op_cap = 0;
    s->vert_count = s->vert_cap = 0;
    s->bytes = 0;
}

/* Return a VALID segment to the pool, bumping the generation so any
 * outstanding handle fails validation from now on. */
static void slot_evict(flux_canvas *c, flux_canvas_record_slot *s) {
    c->record_bytes -= s->bytes;
    slot_free_contents(c, s);
    s->state = FLUX_RECORD_SLOT_FREE;
    s->generation++;
}

static bool slot_on_stack(const flux_canvas *c, const flux_canvas_record_slot *s) {
    for (uint32_t i = 0; i < c->record_depth; ++i)
        if (c->record_stack[i] == s)
            return true;
    return false;
}

static flux_canvas_record_slot *slot_alloc(flux_canvas *c) {
    if (!c->record_slots) {
        c->record_slots =
            flux_canvas_alloc(c->device, FLUX_CANVAS_RECORD_SLOT_CAP * sizeof(*c->record_slots));
        if (!c->record_slots)
            return nullptr;
    }
    for (uint32_t i = 0; i < FLUX_CANVAS_RECORD_SLOT_CAP; ++i)
        if (c->record_slots[i].state == FLUX_RECORD_SLOT_FREE)
            return &c->record_slots[i];

    /* Pool full: reclaim the least-recently-used VALID segment. Slots on
     * the recording stack are untouchable. */
    flux_canvas_record_slot *lru = nullptr;
    for (uint32_t i = 0; i < FLUX_CANVAS_RECORD_SLOT_CAP; ++i) {
        flux_canvas_record_slot *s = &c->record_slots[i];
        if (s->state != FLUX_RECORD_SLOT_VALID || slot_on_stack(c, s))
            continue;
        if (!lru || s->last_used < lru->last_used)
            lru = s;
    }
    if (lru)
        slot_evict(c, lru);
    return lru;
}

void canvas_record_pool_destroy(flux_canvas *c) {
    if (!c->record_slots)
        return;
    for (uint32_t i = 0; i < FLUX_CANVAS_RECORD_SLOT_CAP; ++i)
        slot_free_contents(c, &c->record_slots[i]);
    flux_canvas_free(c->device, c->record_slots);
    c->record_slots = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Capture                                                           */
/* ------------------------------------------------------------------ */

static void slot_poison(flux_canvas_record_slot *s) {
    if (s->state == FLUX_RECORD_SLOT_RECORDING)
        s->state = FLUX_RECORD_SLOT_POISONED;
}

/* Grow `*buf` (element size `esz`) to `need` elements. Returns false on
 * allocation failure; the old buffer stays valid for slot_free_contents. */
static bool slot_grow(flux_canvas *c, void **buf, uint32_t *cap, uint32_t need, size_t esz) {
    uint32_t new_cap = *cap ? *cap : 16u;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2u)
            return false;
        new_cap *= 2u;
    }
    void *nbuf = flux_canvas_alloc(c->device, (size_t)new_cap * esz);
    if (!nbuf)
        return false;
    if (*buf) {
        memcpy(nbuf, *buf, (size_t)*cap * esz);
        flux_canvas_free(c->device, *buf);
    }
    *buf = nbuf;
    *cap = new_cap;
    return true;
}

/* Append one batch to a recording slot. Poison on overflow/alloc failure:
 * the live draw already went out, only the segment is lost. */
static void slot_capture(flux_canvas *c, flux_canvas_record_slot *s, canvas_pipe_id id,
                         const flux_canvas_push *push, const flux_canvas_vertex *verts,
                         uint32_t vertex_count) {
    if (s->state != FLUX_RECORD_SLOT_RECORDING)
        return;
    if (s->op_count == s->op_cap &&
        !slot_grow(c, (void **)&s->ops, &s->op_cap, s->op_count + 1, sizeof(*s->ops))) {
        slot_poison(s);
        return;
    }
    if (s->vert_count + vertex_count > s->vert_cap &&
        !slot_grow(c, (void **)&s->verts, &s->vert_cap, s->vert_count + vertex_count,
                   sizeof(*s->verts))) {
        slot_poison(s);
        return;
    }

    size_t bytes = (size_t)s->op_cap * sizeof(*s->ops) + (size_t)s->vert_cap * sizeof(*s->verts);
    if (bytes > FLUX_CANVAS_RECORD_SEG_BUDGET) {
        slot_poison(s);
        return;
    }
    s->bytes = bytes;

    flux_record_op *op = &s->ops[s->op_count++];
    op->push = *push;
    op->scissor = c->states[c->state_top].scissor;
    op->pipe_id = (uint32_t)id;
    op->blend = (uint32_t)c->pending_blend;
    op->vert_offset = s->vert_count;
    op->vert_count = vertex_count;
    op->host_atlas = c->pending_host_atlas;
    op->host_atlas_w = c->pending_host_atlas_w;
    op->host_atlas_h = c->pending_host_atlas_h;
    memcpy(s->verts + s->vert_count, verts, (size_t)vertex_count * sizeof(*verts));
    s->vert_count += vertex_count;
}

void canvas_record_retain_image(flux_canvas *c, flux_image *img) {
    if (!c || !c->device || !img || c->record_depth == 0)
        return;
    for (uint32_t d = 0; d < c->record_depth; ++d) {
        flux_canvas_record_slot *s = c->record_stack[d];
        if (s->state != FLUX_RECORD_SLOT_RECORDING)
            continue;
        uint32_t i = 0;
        while (i < s->image_count && s->images[i] != img)
            ++i;
        if (i < s->image_count)
            continue; /* already retained */
        if (s->image_count == FLUX_CANVAS_RECORD_IMG_CAP) {
            slot_poison(s);
            continue;
        }
        s->images[s->image_count++] = flux_image_retain(img);
    }
}

void canvas_record_retain_sampler(flux_canvas *c, flux_sampler *sampler) {
    if (!c || !c->device || !sampler || c->record_depth == 0)
        return;
    for (uint32_t d = 0; d < c->record_depth; ++d) {
        flux_canvas_record_slot *s = c->record_stack[d];
        if (s->state != FLUX_RECORD_SLOT_RECORDING)
            continue;
        uint32_t i = 0;
        while (i < s->sampler_count && s->samplers[i] != sampler)
            ++i;
        if (i < s->sampler_count)
            continue;
        if (s->sampler_count == FLUX_CANVAS_RECORD_SAMPLER_CAP) {
            slot_poison(s);
            continue;
        }
        s->samplers[s->sampler_count++] = flux_sampler_retain(sampler);
    }
}

/* ------------------------------------------------------------------ */
/*  Submission choke point                                            */
/* ------------------------------------------------------------------ */

void canvas_emit(flux_canvas *c, canvas_pipe_id id, const flux_canvas_push *push,
                 const flux_canvas_vertex *verts, uint32_t vertex_count) {
    for (uint32_t d = 0; d < c->record_depth; ++d)
        slot_capture(c, c->record_stack[d], id, push, verts, vertex_count);
    c->backend->submit(c->backend, c, id, push, verts, vertex_count);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

bool flux_canvas_begin_record(flux_canvas *c) {
    if (!c || !c->recording) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "flux_canvas_begin_record outside begin/end frame");
        return false;
    }
    if (c->record_depth >= FLUX_CANVAS_RECORD_DEPTH_CAP) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "flux_canvas_begin_record nesting too deep");
        return false;
    }
    flux_canvas_record_slot *s = slot_alloc(c);
    if (!s) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "flux_canvas_begin_record: no slot");
        return false;
    }
    s->owner = c;
    s->state = FLUX_RECORD_SLOT_RECORDING;
    s->anchor_transform = c->states[c->state_top].transform;
    s->anchor_scissor = c->states[c->state_top].scissor;
    s->fb_w = c->fb_width;
    s->fb_h = c->fb_height;
    s->op_count = s->vert_count = s->image_count = s->sampler_count = 0;
    s->bytes = 0;
    c->record_stack[c->record_depth++] = s;
    return true;
}

flux_canvas_record flux_canvas_end_record(flux_canvas *c) {
    flux_canvas_record null_rec = FLUX_CANVAS_RECORD_INIT;
    if (!c || c->record_depth == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "flux_canvas_end_record without begin_record");
        return null_rec;
    }
    flux_canvas_record_slot *s = c->record_stack[--c->record_depth];
    if (s->state == FLUX_RECORD_SLOT_POISONED) {
        slot_free_contents(c, s);
        s->state = FLUX_RECORD_SLOT_FREE;
        s->generation++;
        return null_rec;
    }
    s->state = FLUX_RECORD_SLOT_VALID;
    s->last_used = ++c->record_tick;
    c->record_bytes += s->bytes;
    c->records_created++;

    /* Canvas-wide byte budget: evict least-recently-used segments until
     * the total fits. The just-finished segment has the newest tick, so
     * it is evicted last (and only when it alone exceeds the budget). */
    while (c->record_bytes > FLUX_CANVAS_RECORD_TOTAL_BUDGET) {
        flux_canvas_record_slot *lru = nullptr;
        for (uint32_t i = 0; i < FLUX_CANVAS_RECORD_SLOT_CAP; ++i) {
            flux_canvas_record_slot *v = &c->record_slots[i];
            if (v->state != FLUX_RECORD_SLOT_VALID || slot_on_stack(c, v))
                continue;
            if (!lru || v->last_used < lru->last_used)
                lru = v;
        }
        if (!lru)
            break;
        slot_evict(c, lru);
    }
    return (flux_canvas_record){.slot = s, .generation = s->generation};
}

bool flux_canvas_replay(flux_canvas *c, flux_canvas_record rec) {
    if (!c || !rec.slot || !c->recording)
        return false;
    flux_canvas_record_slot *s = rec.slot;
    if (s->owner != c || s->state != FLUX_RECORD_SLOT_VALID || s->generation != rec.generation)
        return false;

    /* Anchor check: recorded vertices and push constants are baked in
     * physical pixels, so the segment is only faithful under the exact
     * state it was recorded against. */
    flux_canvas_state *cur = &c->states[c->state_top];
    if (s->fb_w != c->fb_width || s->fb_h != c->fb_height)
        return false;
    if (memcmp(&s->anchor_transform, &cur->transform, sizeof(flux_mat3x2)) != 0)
        return false;
    if (memcmp(&s->anchor_scissor, &cur->scissor, sizeof(flux_recti)) != 0)
        return false;
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (!canvas_track_foreign_image(c, s->images[i]))
            return false;
    }

    const flux_recti saved_scissor = cur->scissor;
    const flux_blend_mode saved_blend = c->pending_blend;
    const uint8_t *saved_atlas = c->pending_host_atlas;
    const uint32_t saved_atlas_w = c->pending_host_atlas_w;
    const uint32_t saved_atlas_h = c->pending_host_atlas_h;

    for (uint32_t i = 0; i < s->op_count; ++i) {
        const flux_record_op *op = &s->ops[i];
        /* The CPU backend reads states[top].scissor directly, so the
         * recorded scissor must be poked into the live state (restored
         * below), not just sent to the backend. */
        cur->scissor = op->scissor;
        c->backend->set_scissor(c->backend, c, op->scissor);
        c->pending_blend = (flux_blend_mode)op->blend;
        c->pending_host_atlas = op->host_atlas;
        c->pending_host_atlas_w = op->host_atlas_w;
        c->pending_host_atlas_h = op->host_atlas_h;
        /* A replay inside an active recording is captured too, so a
         * re-recording ancestor ends up with a complete segment even
         * when an unchanged child took the replay path. */
        canvas_emit(c, (canvas_pipe_id)op->pipe_id, &op->push, s->verts + op->vert_offset,
                    op->vert_count);
    }

    cur->scissor = saved_scissor;
    c->backend->set_scissor(c->backend, c, saved_scissor);
    c->pending_blend = saved_blend;
    c->pending_host_atlas = saved_atlas;
    c->pending_host_atlas_w = saved_atlas_w;
    c->pending_host_atlas_h = saved_atlas_h;

    s->last_used = ++c->record_tick;
    c->records_replayed++;
    return true;
}

void flux_canvas_record_release(flux_canvas *c, flux_canvas_record rec) {
    if (!c || !rec.slot)
        return;
    flux_canvas_record_slot *s = rec.slot;
    if (s->owner != c || s->state != FLUX_RECORD_SLOT_VALID || s->generation != rec.generation)
        return;
    slot_evict(c, s);
}

uint64_t flux_canvas_records_created(const flux_canvas *c) {
    return c ? c->records_created : 0;
}

uint64_t flux_canvas_records_replayed(const flux_canvas *c) {
    return c ? c->records_replayed : 0;
}
