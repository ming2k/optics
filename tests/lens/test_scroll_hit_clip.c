/* test_scroll_hit_clip.c — children scrolled out of a scroll viewport
 * must not be hoverable/clickable: the render clips them, hit-testing
 * must clip identically. Covers both scroll containers and table rows. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static const char *ROW_LABELS[20] = {"row-0",  "row-1",  "row-2",  "row-3",  "row-4",
                                     "row-5",  "row-6",  "row-7",  "row-8",  "row-9",
                                     "row-10", "row-11", "row-12", "row-13", "row-14",
                                     "row-15", "row-16", "row-17", "row-18", "row-19"};

static const char *cell_fn(void *user, int row, int col) {
    static char buf[32];
    (void)user;
    snprintf(buf, sizeof buf, "r%dc%d", row, col);
    return buf;
}

static const lens_table_column COLS[] = {
    {.title = "Title", .width = 0, .align = LENS_START},
};

/* A button beyond the scroll viewport's bottom edge is invisible; input
 * landing on its arranged rect must not reach it. After scrolling down
 * it becomes visible at that spot and works. */
static void test_scroll_clips_hit_testing(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool clicked = false;
    bool hovered = false;

    /* Frame 1: establish geometry (viewport ~60px, 20 buttons). */
    lens_begin(ui, &IN0);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        if (lens_button(ui, ROW_LABELS[i]))
            clicked = true;
    }
    lens_scroll_end(ui);
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 2: press+release at y=200 — inside a folded button's rect,
     * far below the viewport. */
    lens_input in = IN0;
    in.cursor = (flux_point){30.0f, 200.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        if (lens_button(ui, ROW_LABELS[i])) {
            clicked = true;
        }
        if (i == 0)
            hovered = lens_get_response(ui).hovered;
    }
    lens_scroll_end(ui);
    lens_end(ui);
    CHECK(!clicked);
    in = IN0;
    in.cursor = (flux_point){30.0f, 200.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        if (lens_button(ui, ROW_LABELS[i]))
            clicked = true;
    }
    lens_scroll_end(ui);
    lens_end(ui);
    CHECK(!clicked);
    (void)hovered;

    /* Scroll to the bottom, then the same point hits the visible row. */
    for (int frame = 0; frame < 3; ++frame) {
        in = IN0;
        in.cursor = (flux_point){30.0f, 30.0f};
        in.scroll_y = -5.0f;
        lens_begin(ui, &in);
        lens_size(ui, 200.0f, 60.0f);
        lens_scroll_begin(ui, "list");
        for (int i = 0; i < 20; ++i)
            (void)lens_button(ui, ROW_LABELS[i]);
        lens_scroll_end(ui);
        lens_end(ui);
    }
    in = IN0;
    in.cursor = (flux_point){30.0f, 30.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i)
        (void)lens_button(ui, ROW_LABELS[i]);
    lens_scroll_end(ui);
    lens_end(ui);
    in = IN0;
    in.cursor = (flux_point){30.0f, 30.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        if (lens_button(ui, ROW_LABELS[i]))
            clicked = true;
    }
    lens_scroll_end(ui);
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

/* Table rows below the visible body must not be selected. */
static void test_table_clips_row_hit_testing(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_table_result r = {0};
    /* Frame 1: establish geometry. */
    lens_begin(ui, &IN0);
    lens_size(ui, 200.0f, 60.0f);
    r = lens_table(ui, "songs", COLS, 1, 20, cell_fn, NULL,
                   (lens_table_opts){.row_height = 28, .selectable = true});
    lens_end(ui);
    CHECK(r.selected == -1);

    /* Frame 2: press at y=250 — over a folded row's content position,
     * below the 60px table body. */
    lens_input in = IN0;
    in.cursor = (flux_point){30.0f, 250.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    r = lens_table(ui, "songs", COLS, 1, 20, cell_fn, NULL,
                   (lens_table_opts){.row_height = 28, .selectable = true});
    lens_end(ui);
    CHECK(r.selected == -1);

    lens_destroy(ui);
}

int main(void) {
    test_scroll_clips_hit_testing();
    test_table_clips_row_hit_testing();
    return TEST_REPORT();
}
