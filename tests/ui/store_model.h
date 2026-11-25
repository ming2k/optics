/* store_model.h — model-based driver for the ui retained store (ADR-0027).
 *
 * Shared by the deterministic unit test (test_store_model.c) and the
 * libFuzzer harness (fuzz/fuzz_ui_store_model.c). The including binary
 * compiles libs/ui/src/store/store.c directly; this header supplies the
 * few hooks store.c calls (allocation, overflow, node cache/frame reset),
 * so the store runs headless with no GPU, text, or layout.
 *
 * A reference model tracks, per key, whether the store must still hold
 * the node (touched within the last OPTICS_UI_LEAVE_GRACE_FRAMES + 1
 * frames) and which node pointer it returned. After every frame the
 * driver proves:
 *
 *   - the structural invariants (optics_ui__store_validate: power-of-two
 *     capacity, load < 3/4, every node reachable from its home slot, no
 *     duplicate ids, live list threads each slot exactly once);
 *   - agreement with the model: count matches, every live key is found
 *     at the pointer first returned for it (retained nodes never move),
 *     every key reaped this frame is gone;
 *   - capacity stays proportionate: the table never sits far above its
 *     population once the shrink hysteresis has had time to act.
 *
 * Any violation prints the reason and aborts, which is what both a test
 * runner and a fuzzer need. A store bug that loops forever (the inverted
 * shrink of v0.0.86) surfaces as the test or fuzz timeout, and with the
 * validator compiled in, as an abort before the loop is reached.
 *
 * Include exactly once per binary: the hooks are external definitions. */
#ifndef OPTICS_TESTS_UI_STORE_MODEL_H
#define OPTICS_TESTS_UI_STORE_MODEL_H

#include "../../libs/ui/src/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- store.c hooks ---------------------------------------------------- */

static long sm_live_allocations;

void *optics_ui__alloc(optics_ui *ui, size_t bytes) {
    (void)ui;
    void *p = malloc(bytes);
    if (p)
        sm_live_allocations++;
    return p;
}
void optics_ui__free(optics_ui *ui, void *p) {
    (void)ui;
    if (p) {
        sm_live_allocations--;
        free(p);
    }
}
void optics_ui__set_overflow(optics_ui *ui) {
    ui->overflow = true;
}
void optics_ui__node_release_cache(optics_ui *ui, optics_ui_node *n) {
    (void)ui;
    (void)n;
}
void optics_ui__node_reset_frame(optics_ui_node *n) {
    (void)n;
}

/* ---- model ------------------------------------------------------------ */

/* Keys are dense indices; ids are a bijection of them (odd multiplier). */
#define SM_MAX_KEYS (1u << 16)

typedef struct sm_ctx {
    optics_ui *ui;
    uint32_t initial_cap;
    /* Per key: node the store returned (nullptr = model says absent),
     * consecutive frames unseen, and the frame it was last touched. */
    optics_ui_node *node[SM_MAX_KEYS];
    uint32_t unseen[SM_MAX_KEYS];
    uint64_t touched_frame[SM_MAX_KEYS];
    /* Dense list of keys the model holds, with back-index for O(1) removal. */
    uint32_t live[SM_MAX_KEYS];
    uint32_t live_pos[SM_MAX_KEYS];
    uint32_t live_count;
    /* Keys touched in the previous / current frame (for "hold" replays). */
    uint32_t last_frame_keys[SM_MAX_KEYS];
    uint32_t last_frame_count;
    uint32_t this_frame_keys[SM_MAX_KEYS];
    uint32_t this_frame_count;
    /* Frames the population has stayed at or under 1/8 of capacity. */
    uint64_t low_load_frames;
    uint64_t frames;
    uint32_t peak_cap, prev_cap;
    uint32_t checked_epoch;
    /* Work accounting: touches so far, and an optional cap (0 = none)
     * that long holds stop at, so a fuzz input's cost stays bounded. */
    uint64_t touches, touch_budget;
    uint32_t shrinks;
} sm_ctx;

[[noreturn]] static void sm_fail(const sm_ctx *c, const char *what, uint32_t key) {
    fprintf(stderr, "store model: %s (frame=%llu key=%u cap=%u count=%u model=%u)\n", what,
            (unsigned long long)c->frames, key, c->ui->store.cap, c->ui->store.count,
            c->live_count);
    abort();
}

static optics_ui_id sm_id(uint32_t key) {
    /* Odd multiplier: a bijection on uint64, so distinct keys never share
     * an id. +0x51 keeps key 0 off the reserved id 0. */
    return (optics_ui_id)((uint64_t)key * 0x9E3779B97F4A7C15ull + 0x51u);
}

static sm_ctx *sm_create(uint32_t initial_cap) {
    sm_ctx *c = calloc(1, sizeof *c);
    if (!c)
        abort();
    /* optics_ui is large (inline pools); keep it off the stack. */
    c->ui = calloc(1, sizeof *c->ui);
    if (!c->ui)
        abort();
    c->ui->frame = 1;
    if (optics_ui__store_init(c->ui, initial_cap) != OPTICS_GFX_OK)
        sm_fail(c, "store_init failed", 0);
    c->initial_cap = c->ui->store.cap;
    c->peak_cap = c->prev_cap = c->ui->store.cap;
    c->checked_epoch = c->ui->store.epoch - 1; /* prove the first frame */
    return c;
}

static void sm_destroy(sm_ctx *c) {
    optics_ui__store_destroy(c->ui);
    if (sm_live_allocations != 0) {
        fprintf(stderr, "store model: %ld allocations leaked\n", sm_live_allocations);
        abort();
    }
    free(c->ui);
    free(c);
}

static void sm_touch(sm_ctx *c, uint32_t key) {
    key %= SM_MAX_KEYS;
    if (c->touched_frame[key] == c->ui->frame && c->node[key])
        return; /* already touched this frame */
    optics_ui_node *n = optics_ui__store_touch(c->ui, sm_id(key));
    if (!n)
        sm_fail(c, "touch returned nullptr without allocation failure", key);
    if (n->id != sm_id(key))
        sm_fail(c, "touch returned a node with another id", key);
    if (c->node[key]) {
        if (n != c->node[key])
            sm_fail(c, "retained node moved (pointer changed while alive)", key);
    } else {
        c->node[key] = n;
        c->live_pos[key] = c->live_count;
        c->live[c->live_count++] = key;
    }
    c->touches++;
    c->unseen[key] = 0;
    c->touched_frame[key] = c->ui->frame;
    c->this_frame_keys[c->this_frame_count++] = key;
}

/* End the frame: reap, advance the model, prove agreement. */
static void sm_end_frame(sm_ctx *c) {
    optics_ui__store_reap(c->ui);

    /* Model GC: a node unseen for more than the grace window is gone. */
    for (uint32_t i = 0; i < c->live_count;) {
        uint32_t key = c->live[i];
        if (c->touched_frame[key] != c->ui->frame &&
            ++c->unseen[key] > OPTICS_UI_LEAVE_GRACE_FRAMES) {
            if (optics_ui__store_find(c->ui, sm_id(key)))
                sm_fail(c, "node outlived its grace window", key);
            c->node[key] = nullptr;
            uint32_t moved = c->live[--c->live_count];
            c->live[i] = moved;
            c->live_pos[moved] = i;
            continue;
        }
        i++;
    }

    const optics_ui_store *s = &c->ui->store;
    if (s->count != c->live_count)
        sm_fail(c, "store count disagrees with the model", 0);
    /* Full proof after any structural change, and periodically on steady
     * frames (which must not change anything — the epoch says so, and the
     * periodic pass checks that claim too). */
    bool changed = s->epoch != c->checked_epoch;
    if (!changed && c->frames % 64)
        goto proven;
    c->checked_epoch = s->epoch;
    const char *broken = optics_ui__store_validate(c->ui);
    if (broken)
        sm_fail(c, broken, 0);
    for (uint32_t i = 0; i < c->live_count; i++) {
        uint32_t key = c->live[i];
        if (optics_ui__store_find(c->ui, sm_id(key)) != c->node[key])
            sm_fail(c, "live node not found at its retained pointer", key);
    }
proven:

    /* Proportionate capacity. Growth keeps load < 3/4 (validated). The
     * shrink must also act: once load has stayed <= 1/8 long enough for
     * every halving the table could need (one dwell per halving), the
     * table must be within 16x of its population or at the floor. */
    if (s->cap > c->peak_cap)
        c->peak_cap = s->cap;
    if (s->cap < c->prev_cap) {
        /* Hysteresis: a shrink needs SHRINK_FRAMES consecutive low-load
         * frames (this one included, judged at the old capacity), so a
         * population wobbling across 1/8 load never rehashes. */
        if ((uint64_t)s->count * 8 > c->prev_cap ||
            c->low_load_frames + 1 < OPTICS_UI_STORE_SHRINK_FRAMES)
            sm_fail(c, "table shrank before the hysteresis dwell elapsed", 0);
        c->shrinks++;
        c->low_load_frames = 0;
    }
    c->prev_cap = s->cap;
    if ((uint64_t)s->count * 8 <= s->cap)
        c->low_load_frames++;
    else
        c->low_load_frames = 0;
    uint32_t halvings = 0;
    for (uint32_t cap = s->cap; cap > OPTICS_UI_STORE_MIN_CAP; cap >>= 1)
        halvings++;
    uint64_t budget = (uint64_t)(halvings + 1) * (OPTICS_UI_STORE_SHRINK_FRAMES + 1);
    uint32_t floor =
        c->initial_cap > OPTICS_UI_STORE_MIN_CAP ? c->initial_cap : OPTICS_UI_STORE_MIN_CAP;
    if (c->low_load_frames > budget && s->cap > floor && (uint64_t)s->count * 16 < s->cap)
        sm_fail(c, "table never shrank after a sustained population collapse", 0);

    memcpy(c->last_frame_keys, c->this_frame_keys, c->this_frame_count * sizeof(uint32_t));
    c->last_frame_count = c->this_frame_count;
    c->this_frame_count = 0;
    c->ui->frame++;
    c->frames++;
}

static bool sm_over_budget(const sm_ctx *c) {
    return c->touch_budget && c->touches >= c->touch_budget;
}

/* Repeat the previous frame's touch set for `frames` frames, optionally
 * keeping only every `stride`-th key (stride 1 = all). This is what holds
 * a population steady across the 300-frame shrink dwell. */
static void sm_hold(sm_ctx *c, uint32_t frames, uint32_t stride) {
    if (!stride)
        stride = 1;
    uint32_t n = 0;
    static uint32_t keep[SM_MAX_KEYS];
    for (uint32_t i = 0; i < c->last_frame_count; i += stride)
        keep[n++] = c->last_frame_keys[i];
    for (uint32_t f = 0; f < frames && !sm_over_budget(c); f++) {
        for (uint32_t i = 0; i < n; i++)
            sm_touch(c, keep[i]);
        sm_end_frame(c);
    }
}

/* One frame touching keys [first, first + count). */
static void sm_frame_range(sm_ctx *c, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        sm_touch(c, first + i);
    sm_end_frame(c);
}

#endif /* OPTICS_TESTS_UI_STORE_MODEL_H */
