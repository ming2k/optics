/*
 * damage_history — the swapchain rotation repair policy (ADR-0149).
 *
 * ADR-0144 rejected per-image "has this image ever had a complete frame" flags
 * because that describes initialisation, not steady state: one frame after the
 * first full paint every image qualifies, and incremental patches then diverge
 * them (measured 31/34/35/18/18 for one rect across five images).
 *
 * The mechanism that replaces the flag is the write-epoch history exercised here.
 * Every frame commits the regions it wrote; every reacquired image asks for the
 * union of entries newer than the epoch it last presented. Divergence is therefore
 * *repaired* on each acquisition rather than assumed absent.
 *
 * These tests are table-driven and GPU-free on purpose. The three ways this
 * mechanism can fail are all arithmetic, and all three are invisible in a
 * screenshot:
 *
 *   1. An image that sits out more frames than the window retains must fall back
 *      to a full copy — not to a partial union that silently drops the oldest
 *      missed frames.
 *   2. "Already current" and "never synchronised" must resolve differently. Merging
 *      them either copies every frame or leaves an image undefined forever.
 *   3. An intervening full-extent frame must force a full copy for everything
 *      older, because a full write is the one entry with no rectangle list.
 */
#include "damage_history.h"
#include "optics_test.h"

#include <string.h>

#define DEPTH OPTICS_GFX_DAMAGE_HISTORY_DEPTH

static optics_gfx_rect rect(float x, float y) {
    return (optics_gfx_rect){x, y, 10.0f, 10.0f};
}

/* One band per frame, distinct so a mis-indexed entry is visible. */
static uint64_t commit_band(optics_gfx_damage_history *h, float y) {
    const optics_gfx_rect r = rect(0.0f, y);
    return optics_gfx_damage_history_commit(h, &r, 1, false);
}

static void test_never_synchronised_is_full(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    optics_gfx_rect out[64];
    bool full = false;

    /* Before anything is committed. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, OPTICS_GFX_DAMAGE_EPOCH_NONE, out, 64, &full) ==
              0);
    OT_EXPECT(full);

    commit_band(&h, 10.0f);
    /* Still: the sentinel never resolves to a partial answer. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, OPTICS_GFX_DAMAGE_EPOCH_NONE, out, 64, &full) ==
              0);
    OT_EXPECT(full);
}

static void test_already_current_copies_nothing(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    optics_gfx_rect out[64];
    bool full = true;

    /* An image leaving the present at e1 is current: nothing to copy. This is
     * the steady-state case for a double-buffered rotation, so getting it wrong
     * would mean a full copy on most frames. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, out, 64, &full) == 0);
    OT_EXPECT(!full);

    /* "Current" must be distinguishable from "never synchronised", which is why
     * the empty answer carries `full == false` while the sentinel carries true. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, OPTICS_GFX_DAMAGE_EPOCH_NONE, out, 64, &full) ==
              0);
    OT_EXPECT(full);
}

static void test_gap_union(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    (void)commit_band(&h, 30.0f);

    optics_gfx_rect out[64];
    bool full = true;
    /* An image last presented at e1 missed e2 and e3: exactly two bands. */
    const uint32_t n = optics_gfx_damage_history_since(&h, e1, out, 64, &full);
    OT_EXPECT(!full);
    OT_EXPECT(n == 2);
    if (n == 2) {
        /* Order is the ring's, which is chronological for entries newer than
         * `since`. What matters is that both distinct bands are present. */
        const bool has20 = (out[0].y == 20.0f) || (out[1].y == 20.0f);
        const bool has30 = (out[0].y == 30.0f) || (out[1].y == 30.0f);
        OT_EXPECT(has20);
        OT_EXPECT(has30);
    }
}

static void test_full_entry_poisons_everything_older(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    /* A full-extent frame: it has no rectangle list, so any image that predates
     * it cannot be repaired from a union of rects — the middle of the surface
     * changed and nothing names it. */
    OT_EXPECT(optics_gfx_damage_history_commit(&h, nullptr, 0, true) == e1 + 1);

    optics_gfx_rect out[64];
    bool full = false;
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, out, 64, &full) == 0);
    OT_EXPECT(full);

    /* An image that leaves *at* the full frame is still current afterwards. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1 + 1, out, 64, &full) == 0);
    OT_EXPECT(!full);
}

static void test_empty_commit_is_no_change_unless_full_is_explicit(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    /* A pacing or history-repair frame can present without writing a new pixel.
     * Empty + full=false is that no-change epoch; only the explicit `full`
     * verdict widens it to the whole extent. */
    (void)optics_gfx_damage_history_commit(&h, nullptr, 0, false);

    optics_gfx_rect out[64];
    bool full = true;
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, out, 64, &full) == 0);
    OT_EXPECT(!full);

    const uint64_t e3 = optics_gfx_damage_history_commit(&h, nullptr, 0, true);
    OT_EXPECT(optics_gfx_damage_history_since(&h, e3 - 1, out, 64, &full) == 0);
    OT_EXPECT(full);
}

static void test_window_overflow_falls_back_to_full(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t first = commit_band(&h, 0.0f);
    /* Sit out one more frame than the window retains, so the entries it missed
     * have been overwritten. */
    for (uint32_t i = 0; i < DEPTH + 1; i++)
        commit_band(&h, 10.0f * (float)(i + 1));

    optics_gfx_rect out[64];
    bool full = false;
    OT_EXPECT(optics_gfx_damage_history_since(&h, first, out, 64, &full) == 0);
    OT_EXPECT(full);

    /* One frame shorter than the window is still reconstructable. */
    const uint64_t recent = h.committed - DEPTH;
    const uint32_t n = optics_gfx_damage_history_since(&h, recent, out, 64, &full);
    OT_EXPECT(!full);
    OT_EXPECT(n == DEPTH);
}

static void test_capacity_overflow_falls_back_to_full(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    (void)commit_band(&h, 30.0f);

    /* A caller buffer that cannot hold the union gets "full", never a truncated
     * list: the omitted entries have no coordinates to contribute, so any shorter
     * answer under-covers. */
    optics_gfx_rect tiny[1];
    bool full = false;
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, tiny, 1, &full) == 0);
    OT_EXPECT(full);

    /* Zero capacity: the union cannot be described at all. A zero-length array
     * would express the same intent, but it is a GNU extension that -Wpedantic
     * (and therefore the release build's -Werror) rejects — the buffer is
     * irrelevant when the capacity already forbids every write. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, tiny, 0, &full) == 0);
    OT_EXPECT(full);
}

static void test_commit_counts_rects_and_drops_degenerate(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const optics_gfx_rect rects[4] = {
        {0.0f, 0.0f, 10.0f, 10.0f},
        {0.0f, 20.0f, 0.0f, 10.0f}, /* zero width: names no pixel */
        {30.0f, 30.0f, 10.0f, 10.0f},
        {40.0f, 40.0f, 10.0f, -1.0f}, /* negative height: names no pixel */
    };
    (void)optics_gfx_damage_history_commit(&h, rects, 4, false);

    optics_gfx_rect out[64];
    bool full = false;
    const uint32_t n = optics_gfx_damage_history_since(&h, 0, out, 64, &full);
    OT_EXPECT(!full);
    OT_EXPECT(n == 2);
}

static void test_reset_forgets_history(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    optics_gfx_damage_history_reset(&h);

    optics_gfx_rect out[64];
    bool full = false;
    /* The old epoch number is meaningless after a reset: every image is
     * unsynchronised with the new content image by definition. */
    OT_EXPECT(optics_gfx_damage_history_since(&h, e1, out, 64, &full) == 0);
    OT_EXPECT(full);
    OT_EXPECT(h.committed == 0);
}

static bool rects_overlap(optics_gfx_rect a, optics_gfx_rect b) {
    const float ix0 = a.x > b.x ? a.x : b.x;
    const float iy0 = a.y > b.y ? a.y : b.y;
    const float ix1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const float iy1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return ix0 < ix1 && iy0 < iy1;
}

static void test_overlapping_and_duplicate_rects_are_pairwise_disjoint(void) {
    optics_gfx_damage_history h;
    optics_gfx_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 100.0f);

    /* Duplicate rects within a single commit (e.g. a 2-pass frame) collapse to one. */
    const optics_gfx_rect dup[2] = {
        {10.0f, 10.0f, 20.0f, 20.0f},
        {10.0f, 10.0f, 20.0f, 20.0f},
    };
    (void)optics_gfx_damage_history_commit(&h, dup, 2, false);

    /* Partially overlapping rect in the next epoch: [20..40, 20..40] overlaps
     * [10..30, 10..30] on [20..30, 20..30] (area 100), so union area is
     * 400 + 400 - 100 = 700. */
    const optics_gfx_rect cross = {20.0f, 20.0f, 20.0f, 20.0f};
    (void)optics_gfx_damage_history_commit(&h, &cross, 1, false);

    optics_gfx_rect out[64];
    bool full = true;
    const uint32_t n = optics_gfx_damage_history_since(&h, e1, out, 64, &full);
    OT_EXPECT(!full);
    OT_EXPECT(n == 3);

    float total_area = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        total_area += out[i].w * out[i].h;
        for (uint32_t j = i + 1; j < n; j++)
            OT_EXPECT(!rects_overlap(out[i], out[j]));
    }
    OT_EXPECT(total_area == 700.0f);
}

int main(void) {
    test_never_synchronised_is_full();
    test_already_current_copies_nothing();
    test_gap_union();
    test_full_entry_poisons_everything_older();
    test_empty_commit_is_no_change_unless_full_is_explicit();
    test_window_overflow_falls_back_to_full();
    test_capacity_overflow_falls_back_to_full();
    test_commit_counts_rects_and_drops_degenerate();
    test_reset_forgets_history();
    test_overlapping_and_duplicate_rects_are_pairwise_disjoint();

    return OT_REPORT();
}
