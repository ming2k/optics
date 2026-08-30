/* test_reconcile.c — phase transitions: enter/stable/leave/reap (ADR-0004). */

#include "test_helpers.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: button enters */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_end(ui);
    lens_id id = lens_get_response(ui).id;
    CHECK(id != 0);
    lens_node *n = lens_find(ui, id);
    CHECK(n != NULL);
    CHECK(lens_node_phase_of(n) == LENS_NODE_ENTERING);

    /* frame 2: stable */
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_end(ui);
    n = lens_find(ui, id);
    CHECK(n != NULL);
    CHECK(lens_node_phase_of(n) == LENS_NODE_STABLE);

    /* frame 3: leave (not built) */
    lens_begin(ui, &in);
    lens_end(ui);
    n = lens_find(ui, id);
    CHECK(n != NULL); /* still in grace window */
    CHECK(lens_node_phase_of(n) == LENS_NODE_LEAVING);

    /* stay absent until reaped */
    bool found_before_reap = false;
    for (int f = 0; f < 20; f++) {
        lens_begin(ui, &in);
        lens_end(ui);
        if (lens_find(ui, id))
            found_before_reap = true;
    }
    CHECK(found_before_reap);         /* grace kept it for a while */
    CHECK(lens_find(ui, id) == NULL); /* eventually reaped */

    lens_destroy(ui);
    return TEST_REPORT();
}
