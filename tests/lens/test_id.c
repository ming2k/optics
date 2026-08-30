/* test_id.c — identity stability and scoping (ADR-0003). CPU-only. */

#include "test_helpers.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(ui != NULL);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* identity is stable across frames for the same label + scope */
    lens_begin(ui, &in);
    lens_id a1 = lens_current_id(ui, "X");
    lens_end(ui);

    lens_begin(ui, &in);
    lens_id a2 = lens_current_id(ui, "X");
    lens_end(ui);
    CHECK(a1 == a2);
    CHECK(a1 != 0);

    /* a pushed scope changes the id */
    lens_begin(ui, &in);
    lens_id base = lens_current_id(ui, "X");
    lens_push_id(ui, "panel");
    lens_id scoped = lens_current_id(ui, "X");
    lens_pop_id(ui);
    lens_id back = lens_current_id(ui, "X");
    lens_end(ui);
    CHECK(base != scoped);
    CHECK(base == back);

    /* loop indices disambiguate identical labels */
    lens_begin(ui, &in);
    lens_push_id_int(ui, 0);
    lens_id row0 = lens_current_id(ui, "row");
    lens_pop_id(ui);
    lens_push_id_int(ui, 1);
    lens_id row1 = lens_current_id(ui, "row");
    lens_pop_id(ui);
    lens_end(ui);
    CHECK(row0 != row1);

    /* Repeated visible labels under one parent are a caller error and must be
     * diagnosable instead of silently looking like missing layout nodes. */
    lens_begin(ui, &in);
    lens_label(ui, &(lens_label_opts){.text = "duplicate"});
    lens_label(ui, &(lens_label_opts){.text = "duplicate"});
    lens_end(ui);
    CHECK(lens_has_duplicate_ids(ui));

    /* Stable data scopes make identical labels distinct. The diagnostic is
     * frame-local, so a clean frame clears the previous report. */
    lens_begin(ui, &in);
    lens_push_id_int(ui, 41);
    lens_label(ui, &(lens_label_opts){.text = "duplicate"});
    lens_pop_id(ui);
    lens_push_id_int(ui, 42);
    lens_label(ui, &(lens_label_opts){.text = "duplicate"});
    lens_pop_id(ui);
    lens_end(ui);
    CHECK(!lens_has_duplicate_ids(ui));

    /* "##" splits the visible label from the id seed */
    lens_begin(ui, &in);
    lens_id ok_save = lens_current_id(ui, "OK##save");
    lens_id ok_discard = lens_current_id(ui, "OK##discard");
    lens_end(ui);
    CHECK(ok_save != ok_discard);

    lens_destroy(ui);
    return TEST_REPORT();
}
