/* test_id.c — identity stability and scoping (ADR-0003). CPU-only. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* identity is stable across frames for the same label + scope */
    optics_ui_begin(ui, &in);
    optics_ui_id a1 = optics_ui_current_id(ui, "X");
    optics_ui_test_end(ui);

    optics_ui_begin(ui, &in);
    optics_ui_id a2 = optics_ui_current_id(ui, "X");
    optics_ui_test_end(ui);
    OT_EXPECT(a1 == a2);
    OT_EXPECT(a1 != 0);

    /* a pushed scope changes the id */
    optics_ui_begin(ui, &in);
    optics_ui_id base = optics_ui_current_id(ui, "X");
    optics_ui_push_id(ui, "panel");
    optics_ui_id scoped = optics_ui_current_id(ui, "X");
    optics_ui_pop_id(ui);
    optics_ui_id back = optics_ui_current_id(ui, "X");
    optics_ui_test_end(ui);
    OT_EXPECT(base != scoped);
    OT_EXPECT(base == back);

    /* loop indices disambiguate identical labels */
    optics_ui_begin(ui, &in);
    optics_ui_push_id_int(ui, 0);
    optics_ui_id row0 = optics_ui_current_id(ui, "row");
    optics_ui_pop_id(ui);
    optics_ui_push_id_int(ui, 1);
    optics_ui_id row1 = optics_ui_current_id(ui, "row");
    optics_ui_pop_id(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(row0 != row1);

    /* Repeated visible labels under one parent are a caller error and must be
     * diagnosable instead of silently looking like missing layout nodes. */
    optics_ui_begin(ui, &in);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "duplicate"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "duplicate"});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_has_duplicate_ids(ui));

    /* Stable data scopes make identical labels distinct. The diagnostic is
     * frame-local, so a clean frame clears the previous report. */
    optics_ui_begin(ui, &in);
    optics_ui_push_id_int(ui, 41);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "duplicate"});
    optics_ui_pop_id(ui);
    optics_ui_push_id_int(ui, 42);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "duplicate"});
    optics_ui_pop_id(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_has_duplicate_ids(ui));

    /* "##" splits the visible label from the id seed */
    optics_ui_begin(ui, &in);
    optics_ui_id ok_save = optics_ui_current_id(ui, "OK##save");
    optics_ui_id ok_discard = optics_ui_current_id(ui, "OK##discard");
    optics_ui_test_end(ui);
    OT_EXPECT(ok_save != ok_discard);

    optics_ui_release(ui);
    return OT_REPORT();
}
