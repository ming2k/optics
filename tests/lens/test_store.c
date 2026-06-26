/* test_store.c — retained node lifecycle and per-node state (ADR-0004).
 * CPU-only; no device. */

#include "test_helpers.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: a button enters */
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
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
    (void)lens_button(ui, "A");
    lens_end(ui);

    lens_node *n2 = lens_find(ui, aid);
    CHECK(n2 == n); /* retained */
    CHECK(lens_node_phase_of(n2) == LENS_NODE_STABLE);
    int *s2 = (int *)lens_node_state(n2, sizeof(int));
    CHECK(s2 == s);
    CHECK(*s2 == 42); /* persisted */

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

    lens_destroy(ui);
    return TEST_REPORT();
}
