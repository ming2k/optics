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

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

#define DEPTH FLUX_DAMAGE_HISTORY_DEPTH

static flux_rect rect(float x, float y) {
    return (flux_rect){x, y, 10.0f, 10.0f};
}

/* One band per frame, distinct so a mis-indexed entry is visible. */
static uint64_t commit_band(flux_damage_history *h, float y) {
    const flux_rect r = rect(0.0f, y);
    return flux_damage_history_commit(h, &r, 1, false);
}

static void test_never_synchronised_is_full(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    flux_rect out[64];
    bool full = false;

    /* Before anything is committed. */
    CHECK(flux_damage_history_since(&h, FLUX_DAMAGE_EPOCH_NONE, out, 64, &full) == 0);
    CHECK(full);

    commit_band(&h, 10.0f);
    /* Still: the sentinel never resolves to a partial answer. */
    CHECK(flux_damage_history_since(&h, FLUX_DAMAGE_EPOCH_NONE, out, 64, &full) == 0);
    CHECK(full);
}

static void test_already_current_copies_nothing(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    flux_rect out[64];
    bool full = true;

    /* An image leaving the present at e1 is current: nothing to copy. This is
     * the steady-state case for a double-buffered rotation, so getting it wrong
     * would mean a full copy on most frames. */
    CHECK(flux_damage_history_since(&h, e1, out, 64, &full) == 0);
    CHECK(!full);

    /* "Current" must be distinguishable from "never synchronised", which is why
     * the empty answer carries `full == false` while the sentinel carries true. */
    CHECK(flux_damage_history_since(&h, FLUX_DAMAGE_EPOCH_NONE, out, 64, &full) == 0);
    CHECK(full);
}

static void test_gap_union(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    (void)commit_band(&h, 30.0f);

    flux_rect out[64];
    bool full = true;
    /* An image last presented at e1 missed e2 and e3: exactly two bands. */
    const uint32_t n = flux_damage_history_since(&h, e1, out, 64, &full);
    CHECK(!full);
    CHECK(n == 2);
    if (n == 2) {
        /* Order is the ring's, which is chronological for entries newer than
         * `since`. What matters is that both distinct bands are present. */
        const bool has20 = (out[0].y == 20.0f) || (out[1].y == 20.0f);
        const bool has30 = (out[0].y == 30.0f) || (out[1].y == 30.0f);
        CHECK(has20);
        CHECK(has30);
    }
}

static void test_full_entry_poisons_everything_older(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    /* A full-extent frame: it has no rectangle list, so any image that predates
     * it cannot be repaired from a union of rects — the middle of the surface
     * changed and nothing names it. */
    CHECK(flux_damage_history_commit(&h, nullptr, 0, true) == e1 + 1);

    flux_rect out[64];
    bool full = false;
    CHECK(flux_damage_history_since(&h, e1, out, 64, &full) == 0);
    CHECK(full);

    /* An image that leaves *at* the full frame is still current afterwards. */
    CHECK(flux_damage_history_since(&h, e1 + 1, out, 64, &full) == 0);
    CHECK(!full);
}

static void test_empty_commit_means_full_not_nothing(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    /* A frame that commits no rectangles has no narrower description of what it
     * wrote. Reading that as "nothing changed" is the one direction that leaves
     * stale pixels on screen, so it must read as "everything". */
    (void)flux_damage_history_commit(&h, nullptr, 0, false);

    flux_rect out[64];
    bool full = false;
    CHECK(flux_damage_history_since(&h, e1, out, 64, &full) == 0);
    CHECK(full);
}

static void test_window_overflow_falls_back_to_full(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t first = commit_band(&h, 0.0f);
    /* Sit out one more frame than the window retains, so the entries it missed
     * have been overwritten. */
    for (uint32_t i = 0; i < DEPTH + 1; i++)
        commit_band(&h, 10.0f * (float)(i + 1));

    flux_rect out[64];
    bool full = false;
    CHECK(flux_damage_history_since(&h, first, out, 64, &full) == 0);
    CHECK(full);

    /* One frame shorter than the window is still reconstructable. */
    const uint64_t recent = h.committed - DEPTH;
    const uint32_t n = flux_damage_history_since(&h, recent, out, 64, &full);
    CHECK(!full);
    CHECK(n == DEPTH);
}

static void test_capacity_overflow_falls_back_to_full(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    (void)commit_band(&h, 30.0f);

    /* A caller buffer that cannot hold the union gets "full", never a truncated
     * list: the omitted entries have no coordinates to contribute, so any shorter
     * answer under-covers. */
    flux_rect tiny[1];
    bool full = false;
    CHECK(flux_damage_history_since(&h, e1, tiny, 1, &full) == 0);
    CHECK(full);

    /* Zero capacity: the union cannot be described at all. A zero-length array
     * would express the same intent, but it is a GNU extension that -Wpedantic
     * (and therefore the release build's -Werror) rejects — the buffer is
     * irrelevant when the capacity already forbids every write. */
    CHECK(flux_damage_history_since(&h, e1, tiny, 0, &full) == 0);
    CHECK(full);
}

static void test_commit_counts_rects_and_drops_degenerate(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const flux_rect rects[4] = {
        {0.0f, 0.0f, 10.0f, 10.0f},
        {0.0f, 20.0f, 0.0f, 10.0f}, /* zero width: names no pixel */
        {30.0f, 30.0f, 10.0f, 10.0f},
        {40.0f, 40.0f, 10.0f, -1.0f}, /* negative height: names no pixel */
    };
    (void)flux_damage_history_commit(&h, rects, 4, false);

    flux_rect out[64];
    bool full = false;
    const uint32_t n = flux_damage_history_since(&h, 0, out, 64, &full);
    CHECK(!full);
    CHECK(n == 2);
}

static void test_reset_forgets_history(void) {
    flux_damage_history h;
    flux_damage_history_reset(&h);
    const uint64_t e1 = commit_band(&h, 10.0f);
    (void)commit_band(&h, 20.0f);
    flux_damage_history_reset(&h);

    flux_rect out[64];
    bool full = false;
    /* The old epoch number is meaningless after a reset: every image is
     * unsynchronised with the new content image by definition. */
    CHECK(flux_damage_history_since(&h, e1, out, 64, &full) == 0);
    CHECK(full);
    CHECK(h.committed == 0);
}

int main(void) {
    test_never_synchronised_is_full();
    test_already_current_copies_nothing();
    test_gap_union();
    test_full_entry_poisons_everything_older();
    test_empty_commit_means_full_not_nothing();
    test_window_overflow_falls_back_to_full();
    test_capacity_overflow_falls_back_to_full();
    test_commit_counts_rects_and_drops_degenerate();
    test_reset_forgets_history();

    fprintf(stderr, "damage_history: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
