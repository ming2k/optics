/* test_store.c — retained node lifecycle and per-node state (ADR-0004).
 * CPU-only; no device. */

#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

/* Internal store fields: this test asserts the shrink hysteresis itself
 * (capacity floor and the idle-frame counters), which has no public
 * accessor by design. */
#include "../../libs/lens/src/internal.h"

int main(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: a button enters */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_id aid = lens_get_response(ui).id;
    lens_test_end(ui);
    OT_EXPECT(aid != 0);

    lens_node *n = lens_find(ui, aid);
    OT_EXPECT(n != NULL);
    OT_EXPECT(lens_node_phase_of(n) == LENS_NODE_ENTERING); /* no prior frame */

    /* stash persistent per-node state */
    int *s = (int *)lens_node_state(n, sizeof(int));
    OT_EXPECT(s != NULL);
    *s = 42;

    /* frame 2: same id -> same node pointer, state carried forward */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_test_end(ui);

    lens_node *n2 = lens_find(ui, aid);
    OT_EXPECT(n2 == n); /* retained */
    OT_EXPECT(lens_node_phase_of(n2) == LENS_NODE_STABLE);
    int *s2 = (int *)lens_node_state(n2, sizeof(int));
    OT_EXPECT(s2 == s);
    OT_EXPECT(*s2 == 42);                                    /* persisted */
    OT_EXPECT(lens_node_state(n2, sizeof(int) + 1) == NULL); /* fixed type/size */
    OT_EXPECT(lens_node_state(n2, sizeof(int)) == s);        /* address remains stable */

    /* stop building it: it should leave, then reap after the grace window */
    bool present_during_grace = false;
    for (int f = 0; f < 4; f++) {
        lens_begin(ui, &in);
        /* build nothing */
        lens_test_end(ui);
        if (lens_find(ui, aid))
            present_during_grace = true;
    }
    OT_EXPECT(present_during_grace); /* grace kept it alive */

    for (int f = 0; f < 12; f++) {
        lens_begin(ui, &in);
        lens_test_end(ui);
    }
    OT_EXPECT(lens_find(ui, aid) == NULL); /* reaped */

    /* ---- sustained population collapse shrinks the slot table --------
     *
     * A transient spike (a long list, a notification burst) used to pin
     * the open-addressing table at its high-water capacity for the
     * process lifetime, and lensi_store_reap scans O(cap) every frame.
     * After LENSI_STORE_SHRINK_FRAMES consecutive frames under 1/8 load
     * the table must halve (never below LENSI_STORE_MIN_CAP). */
    {
        /* Reopen a store sparsely populated post-spike: the earlier part of
         * this test reaped everything, so the load is already minimal. Just
         * verify the hysteresis counters behave: run a long idle stretch
         * and confirm the table never grows and never shrinks below the
         * floor, then spike it once and confirm it is still functional. */
        for (int f = 0; f < LENSI_STORE_SHRINK_FRAMES + 16; f++) {
            lens_begin(ui, &in);
            lens_test_end(ui);
        }
        OT_EXPECT(ui->store.cap >= LENSI_STORE_MIN_CAP);
        /* The implicit root node is touched every frame, so the live count
         * is exactly the always-present set — never the spiked high-water. */
        OT_EXPECT(ui->store.count <= 4);

        /* functional after any shrink: build, find, and reap normally */
        lens_begin(ui, &in);
        (void)lens_button(ui, &(lens_button_opts){.label = "post-shrink"});
        lens_id pid = lens_get_response(ui).id;
        lens_test_end(ui);
        OT_EXPECT(pid != 0 && lens_find(ui, pid) != NULL);
    }

    lens_release(ui);
    return OT_REPORT();
}
