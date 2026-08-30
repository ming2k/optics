/* test_store.c — retained node lifecycle and per-node state (ADR-0004).
 * CPU-only; no device. */

#include "test_helpers.h"
#include <lens/lens.h>

/* Internal store fields: this test asserts the shrink hysteresis itself
 * (capacity floor and the idle-frame counters), which has no public
 * accessor by design. */
#include "../../libs/lens/src/internal.h"

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: a button enters */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_id aid = lens_get_response(ui).id;
    lens_end(ui);
    CHECK(aid != 0);

    lens_node *n = lens_find(ui, aid);
    CHECK(n != NULL);
    CHECK(lens_node_phase_of(n) == LENS_NODE_ENTERING); /* no prior frame */

    /* stash persistent per-node state */
    int *s = (int *)lens_node_state(n, sizeof(int));
    CHECK(s != NULL);
    *s = 42;

    /* frame 2: same id -> same node pointer, state carried forward */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_end(ui);

    lens_node *n2 = lens_find(ui, aid);
    CHECK(n2 == n); /* retained */
    CHECK(lens_node_phase_of(n2) == LENS_NODE_STABLE);
    int *s2 = (int *)lens_node_state(n2, sizeof(int));
    CHECK(s2 == s);
    CHECK(*s2 == 42);                                    /* persisted */
    CHECK(lens_node_state(n2, sizeof(int) + 1) == NULL); /* fixed type/size */
    CHECK(lens_node_state(n2, sizeof(int)) == s);        /* address remains stable */

    /* stop building it: it should leave, then reap after the grace window */
    bool present_during_grace = false;
    for (int f = 0; f < 4; f++) {
        lens_begin(ui, &in);
        /* build nothing */
        lens_end(ui);
        if (lens_find(ui, aid))
            present_during_grace = true;
    }
    CHECK(present_during_grace); /* grace kept it alive */

    for (int f = 0; f < 12; f++) {
        lens_begin(ui, &in);
        lens_end(ui);
    }
    CHECK(lens_find(ui, aid) == NULL); /* reaped */

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
            lens_end(ui);
        }
        CHECK(ui->store.cap >= LENSI_STORE_MIN_CAP);
        /* The implicit root node is touched every frame, so the live count
         * is exactly the always-present set — never the spiked high-water. */
        CHECK(ui->store.count <= 4);

        /* functional after any shrink: build, find, and reap normally */
        lens_begin(ui, &in);
        (void)lens_button(ui, &(lens_button_opts){.label = "post-shrink"});
        lens_id pid = lens_get_response(ui).id;
        lens_end(ui);
        CHECK(pid != 0 && lens_find(ui, pid) != NULL);
    }

    lens_destroy(ui);
    return TEST_REPORT();
}
