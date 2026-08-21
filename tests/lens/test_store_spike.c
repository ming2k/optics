/*
 * test_store_spike.c — lens store GC across a population spike.
 *
 * Regression test for the live-list reap rework, which introduced (and
 * this test caught) two bug classes:
 *
 *  1. Iteration invalidation: a reaped node's probe-cluster clear can
 *     unhook the entry the reap iterator is about to visit. The walk
 *     must re-anchor at the head and never dereference a cleared slot —
 *     the broken version crashed inside lensi_store_reap on the frame
 *     after the grace window expired, for spike sizes as small as 70.
 *
 *  2. Link loss: unhooking displaced cluster entries one at a time and
 *     re-inserting immediately could, when a rehash fired mid-cluster,
 *     rebuild the live list from a half-updated state — nodes silently
 *     fell off the list and leaked (slots cleared, nodes never freed).
 *     ASan leak detection at exit catches any survivor.
 *
 * Also exercises the dwell between spike and shrink-hysteresis, where
 * the table sits at spike capacity with a tiny live population. CPU
 * only; headless lens (no device).
 */
#include "test_helpers.h"
#include <lens/lens.h>

#define GRACE_FRAMES 8 /* LENSI_LEAVE_GRACE_FRAMES (internal.h); see also below */

static bool g_seen_button;
static void saw_button(const lens_semantics *s, flux_rect b, lens_id id, lens_id parent,
                       void *user) {
    (void)b;
    (void)id;
    (void)parent;
    (void)user;
    if (s->role == LENS_ROLE_BUTTON)
        g_seen_button = true;
}

/* Build `count` buttons in one frame, then run `idle` empty frames so
 * the store passes through ENTERING -> LEAVING -> reaped, including the
 * cluster clears around each reaped slot. */
static void spike_and_dwell(lens *ui, int count, int idle_frames) {
    lens_begin(ui, &(lens_input){.size = sizeof(lens_input)});
    for (int i = 0; i < count; i++) {
        char label[32];
        snprintf(label, sizeof label, "spike##%d", i);
        lens_button(ui, label);
    }
    lens_end(ui);
    /* Per-frame arena overflow is acceptable and expected for the big
     * spikes: draw calls get dropped, but the retained store still
     * tracks every node it created, which is what this test exercises. */
    for (int f = 0; f < idle_frames; f++) {
        lens_begin(ui, &(lens_input){.size = sizeof(lens_input)});
        lens_end(ui);
    }
}

int main(void) {
    /* Small arena on purpose: the overflow path is part of the stress. */
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.arena_bytes = 64u * 1024u}, &ui) == FLUX_OK);

    /* 1. Spike sizes around the growth thresholds (initial cap 256 at
     *    0.75 load = 191 entries; then 512, 1024, ...). Every size here
     *    crashed the broken reap on the frame after the grace window;
     *    sizes that force a mid-reap rehash exercise bug class 2. */
    static const int sizes[] = {50, 70, 100, 150, 191, 200, 260, 400, 700};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        spike_and_dwell(ui, sizes[k], GRACE_FRAMES + 4);
        CHECK(k < sizeof sizes / sizeof sizes[0]); /* reached the end alive */
    }

    /* 2. Big spike + dwell: 20 000 nodes forces several growths and a
     *    long cluster-clear tail when the whole spike ages out. */
    spike_and_dwell(ui, 20000, 40);

    /* 3. Re-spike after the dwell: the store must still create, find,
     *    and reap normally (a corrupted list shows up here or at exit). */
    spike_and_dwell(ui, 500, GRACE_FRAMES + 4);

    /* 4. Ordinary frames still work after all the GC: build a button
     *    and verify the retained node carries the button role (the
     *    persistent-widget contract the store exists to provide).
     *    lens_button returns "clicked", which is false without input —
     *    node presence is the surviving-state assertion here. */
    lens_begin(ui, &(lens_input){.size = sizeof(lens_input)});
    lens_row(ui);
    (void)lens_button(ui, "stable##keep");
    lens_end(ui);
    g_seen_button = false;
    lens_accessibility_walk(ui, saw_button, NULL);
    CHECK(g_seen_button);

    lens_destroy(ui);
    return TEST_REPORT();
}
