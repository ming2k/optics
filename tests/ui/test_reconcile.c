/* test_reconcile.c — phase transitions: enter/stable/leave/reap (ADR-0004). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: button enters */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_test_end(ui);
    optics_ui_id id = optics_ui_get_response(ui).id;
    OT_EXPECT(id != 0);
    optics_ui_node *n = optics_ui_find(ui, id);
    OT_EXPECT(n != nullptr);
    OT_EXPECT(optics_ui_node_phase_of(n) == OPTICS_UI_NODE_ENTERING);

    /* frame 2: stable */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_test_end(ui);
    n = optics_ui_find(ui, id);
    OT_EXPECT(n != nullptr);
    OT_EXPECT(optics_ui_node_phase_of(n) == OPTICS_UI_NODE_STABLE);

    /* frame 3: leave (not built) */
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    n = optics_ui_find(ui, id);
    OT_EXPECT(n != nullptr); /* still in grace window */
    OT_EXPECT(optics_ui_node_phase_of(n) == OPTICS_UI_NODE_LEAVING);

    /* stay absent until reaped */
    bool found_before_reap = false;
    for (int f = 0; f < 20; f++) {
        optics_ui_begin(ui, &in);
        optics_ui_test_end(ui);
        if (optics_ui_find(ui, id))
            found_before_reap = true;
    }
    OT_EXPECT(found_before_reap);                 /* grace kept it for a while */
    OT_EXPECT(optics_ui_find(ui, id) == nullptr); /* eventually reaped */

    optics_ui_release(ui);
    return OT_REPORT();
}
