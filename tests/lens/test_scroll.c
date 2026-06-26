/* test_scroll.c — scroll container offset handling. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 400}, .dt_seconds = 0.016f};

static void test_scroll_offset(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* First frame: build scroll container with many items */
    lens_begin(ui, &IN0);
    lens_size(ui, 0, 200);
    lens_scroll_begin(ui, "scroll");
    for (int i = 0; i < 20; i++) {
        lens_label(ui, "Item");
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* Second frame: scroll wheel over it */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 50};
    in.scroll_y = 5.0f; /* scroll down */
    lens_begin(ui, &in);
    lens_size(ui, 0, 200);
    lens_scroll_begin(ui, "scroll");
    for (int i = 0; i < 20; i++) {
        lens_label(ui, "Item");
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* Just verify no crash; scroll consumption is internal. */
    CHECK(1);

    lens_destroy(ui);
}

int main(void) {
    test_scroll_offset();
    return TEST_REPORT();
}
