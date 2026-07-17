/* test_overlay_eclipse.c — a floating layer must swallow interactions aimed
 * at base widgets underneath it, while its own children stay interactive.
 * Covers both lensi_interact widgets (buttons) and widgets with their own
 * hit-testing (table rows, scroll wheel routing). */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static const lens_overlay_opts CARD_OPTS = {
    .pad = 10.0f,
    .gap = 4.0f,
    .bg = 0xFA20202Cu,
    .border = 0x12FFFFFFu,
    .border_width = 1.0f,
    .radius = 12.0f,
    .min_width = 260.0f,
    .cross = LENS_STRETCH,
};

/* Base button at the left; a panel layer (queue-hover-card style, h=0
 * anchor, content-sized) is registered over it. A click landing on the
 * panel must never reach the base button; a click on the panel's own
 * child button must work. */
static void test_panel_layer_eclipses_base_widget(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool base_clicked = false;
    bool card_clicked = false;

    for (int frame = 0; frame < 4; ++frame) {
        lens_input in = IN0;
        if (frame == 2)
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        if (frame == 3)
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        if (frame >= 2)
            in.cursor = (flux_point){40.0f, 40.0f};

        lens_begin(ui, &in);

        /* base content: a full-area button the panel will cover */
        if (lens_button(ui, "covered playlist row"))
            base_clicked = true;

        /* floating card over the same spot (anchor h=0 like wavora's
         * queue hover card) */
        lens_layer_begin(ui, "hover-card", (flux_rect){20.0f, 20.0f, 260.0f, 0.0f}, CARD_OPTS);
        if (lens_button(ui, "card button"))
            card_clicked = true;
        lens_layer_end(ui);

        lens_end(ui);
    }

    CHECK(!base_clicked);
    CHECK(card_clicked);
    lens_destroy(ui);
}

static const char *cell_fn(void *user, int row, int col) {
    static char buf[32];
    (void)user;
    snprintf(buf, sizeof buf, "r%dc%d", row, col);
    return buf;
}

static const lens_table_column COLS[] = {
    {.title = "Title", .width = 0, .align = LENS_START},
};

static lens_table_result build_table(lens *ui, const lens_input *in, bool with_card) {
    lens_begin(ui, in);
    lens_size(ui, 400, 300);
    lens_table_result r = lens_table(ui, "songs", COLS, 1, 6, cell_fn, NULL,
                                     (lens_table_opts){.row_height = 28, .selectable = true});
    if (with_card) {
        lens_layer_begin(ui, "hover-card", (flux_rect){10.0f, 40.0f, 260.0f, 0.0f}, CARD_OPTS);
        lens_label(ui, "details");
        lens_layer_end(ui);
    }
    lens_end(ui);
    return r;
}

/* A selectable table under a floating card must not select or scroll
 * through the card (table rows do their own hit-testing). */
static void test_floating_card_eclipses_table_rows(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish geometry with the card visible. */
    lens_table_result r = build_table(ui, &IN0, true);
    CHECK(r.selected == -1);

    /* Frame 2: press over a covered row. */
    lens_input in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, true);
    CHECK(r.selected == -1);

#ifdef DEBUG_ECLIPSE
    {
        lens_node *root = lens_root(ui);
        lens_node *table = lens_node_first_child(root);
        fprintf(stderr, "dbg table bounds: %.0f %.0f %.0f %.0f\n", lens_node_bounds(table).x,
                lens_node_bounds(table).y, lens_node_bounds(table).w, lens_node_bounds(table).h);
    }
#endif

    /* Frame 3: release. Still no selection. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, true);
    CHECK(r.selected == -1);

    /* Frame 4: wheel over the covered table must not route to it. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.scroll_y = 3.0f;
    (void)build_table(ui, &in, true);

    /* Drain one card-free frame: a vanished layer keeps eclipsing for
     * exactly one frame (same one-frame latency as its appearance). */
    (void)build_table(ui, &IN0, false);

    /* Control: without the card, the same press selects the row. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, false);
    CHECK(r.selected >= 0);

    lens_destroy(ui);
}

/* Wheel scrolling must not reach a scroll area hidden under a card. */
static void test_floating_card_eclipses_scroll_wheel(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    float first_row_y = 0.0f;
    for (int frame = 0; frame < 2; ++frame) {
        lens_input in = IN0;
        if (frame == 1) {
            in.cursor = (flux_point){60.0f, 50.0f};
            in.scroll_y = 3.0f;
        }
        lens_begin(ui, &in);
        lens_size(ui, 200.0f, 60.0f);
        lens_scroll_begin(ui, "list");
        for (int i = 0; i < 20; ++i)
            lens_label(ui, "row");
        lens_scroll_end(ui);
        lens_layer_begin(ui, "hover-card", (flux_rect){10.0f, 10.0f, 260.0f, 0.0f}, CARD_OPTS);
        lens_label(ui, "details");
        lens_layer_end(ui);
        lens_end(ui);

        lens_node *scroll = lens_node_first_child(lens_root(ui));
        CHECK(scroll != NULL);
        lens_node *first_row = lens_node_first_child(scroll);
        CHECK(first_row != NULL);
        float y = lens_node_bounds(first_row).y;
        if (frame == 0)
            first_row_y = y;
        else
            CHECK(y == first_row_y); /* covered: wheel must not move the list */
    }

    /* Drain one card-free frame before the control case. */
    lens_begin(ui, &IN0);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i)
        lens_label(ui, "row");
    lens_scroll_end(ui);
    lens_end(ui);

    /* Control: without the card, the same wheel delta scrolls the list. */
    lens_input in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.scroll_y = -3.0f;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i)
        lens_label(ui, "row");
    lens_scroll_end(ui);
    lens_end(ui);
    lens_node *scroll = lens_node_first_child(lens_root(ui));
    CHECK(scroll != NULL);
    lens_node *first_row = lens_node_first_child(scroll);
    CHECK(first_row != NULL);
    CHECK(lens_node_bounds(first_row).y != first_row_y);

    lens_destroy(ui);
}

int main(void) {
    test_panel_layer_eclipses_base_widget();
    test_floating_card_eclipses_table_rows();
    test_floating_card_eclipses_scroll_wheel();
    return TEST_REPORT();
}
