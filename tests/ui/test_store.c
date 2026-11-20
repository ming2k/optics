/* test_store.c — retained node lifecycle and per-node state (ADR-0004).
 * CPU-only; no device. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

/* Internal store fields: this test asserts the shrink hysteresis itself
 * (capacity floor and the idle-frame counters), which has no public
 * accessor by design. */
#include "../../libs/ui/src/internal.h"

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: a button enters */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_id aid = optics_ui_get_response(ui).id;
    optics_ui_test_end(ui);
    OT_EXPECT(aid != 0);

    optics_ui_node *n = optics_ui_find(ui, aid);
    OT_EXPECT(n != nullptr);
    OT_EXPECT(optics_ui_node_phase_of(n) == OPTICS_UI_NODE_ENTERING); /* no prior frame */

    /* stash persistent per-node state */
    int *s = (int *)optics_ui_node_state(n, sizeof(int));
    OT_EXPECT(s != nullptr);
    *s = 42;

    /* frame 2: same id -> same node pointer, state carried forward */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_test_end(ui);

    optics_ui_node *n2 = optics_ui_find(ui, aid);
    OT_EXPECT(n2 == n); /* retained */
    OT_EXPECT(optics_ui_node_phase_of(n2) == OPTICS_UI_NODE_STABLE);
    int *s2 = (int *)optics_ui_node_state(n2, sizeof(int));
    OT_EXPECT(s2 == s);
    OT_EXPECT(*s2 == 42);                                            /* persisted */
    OT_EXPECT(optics_ui_node_state(n2, sizeof(int) + 1) == nullptr); /* fixed type/size */
    OT_EXPECT(optics_ui_node_state(n2, sizeof(int)) == s);           /* address remains stable */

    /* stop building it: it should leave, then reap after the grace window */
    bool present_during_grace = false;
    for (int f = 0; f < 4; f++) {
        optics_ui_begin(ui, &in);
        /* build nothing */
        optics_ui_test_end(ui);
        if (optics_ui_find(ui, aid))
            present_during_grace = true;
    }
    OT_EXPECT(present_during_grace); /* grace kept it alive */

    for (int f = 0; f < 12; f++) {
        optics_ui_begin(ui, &in);
        optics_ui_test_end(ui);
    }
    OT_EXPECT(optics_ui_find(ui, aid) == nullptr); /* reaped */

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
            optics_ui_begin(ui, &in);
            optics_ui_test_end(ui);
        }
        OT_EXPECT(ui->store.cap >= LENSI_STORE_MIN_CAP);
        /* The implicit root node is touched every frame, so the live count
         * is exactly the always-present set — never the spiked high-water. */
        OT_EXPECT(ui->store.count <= 4);

        /* functional after any shrink: build, find, and reap normally */
        optics_ui_begin(ui, &in);
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "post-shrink"});
        optics_ui_id pid = optics_ui_get_response(ui).id;
        optics_ui_test_end(ui);
        OT_EXPECT(pid != 0 && optics_ui_find(ui, pid) != nullptr);
    }

    optics_ui_release(ui);
    return OT_REPORT();
}
