/* test_tab_strip.c — tab strip drag-to-reorder verification. */

#include "test_helpers.h"
#include <lens/icon.h>
#include <lens/patterns.h>
#include <stdio.h>
#include <string.h>

/* Build a three-tab strip, returns nothing; helpers share one layout fn so
 * ids stay stable across frames. */
static void build_strip(lens *ui, uint32_t active) {
    lens_tab_item tabs[3] = {
        {.title = "Alpha", .icon = LENS_ICON_INVALID, .closable = true},
        {.title = "Beta", .icon = LENS_ICON_INVALID, .closable = true},
        {.title = "Gamma", .icon = LENS_ICON_INVALID, .closable = true},
    };
    lens_tab_strip_opts opts = {.height = 36.0f,
                                .min_tab_width = 80.0f,
                                .max_tab_width = 120.0f,
                                .show_new_button = true,
                                .close_icon = LENS_ICON_INVALID,
                                .new_icon = LENS_ICON_INVALID};
    lens_tab_strip(ui, "##main-tabs", tabs, 3, active, &opts);
}

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(NULL, &ui) == FLUX_OK);
    CHECK(ui != NULL);

    lens_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: neutral build so tab rects exist for interaction (ADR-0029). */
    in.cursor = (flux_point){0, 0};
    lens_begin(ui, &in);
    build_strip(ui, 0);
    test_end(ui);

    /* Frame 2: press on the first tab — the strip must flag the press so a
     * CSD host suppresses the window-move grab (tab drags take priority). */
    in.cursor = (flux_point){10, 18};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_tab_action a = {0};
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    /* A press without release is not a select yet. */
    CHECK(a.kind == LENS_TAB_ACTION_NONE);
    CHECK(a.pressed_on_tab);
    test_end(ui);

    /* Frame 3: small movement below the threshold — still no action. */
    in.cursor = (flux_point){16, 22};
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_NONE);
    test_end(ui);

    /* Frame 4: movement past the 14px threshold — drag becomes active but
     * nothing fires until release. Cursor far right: predicted slot = 2
     * (post-removal coordinates). */
    in.cursor = (flux_point){300, 18};
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_NONE);
    test_end(ui);

    /* Frame 5: release over the third tab region → MOVE(0 -> 2). */
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_MOVE);
    CHECK(a.index == 0);
    CHECK(a.to == 2);
    test_end(ui);

    /* Frame 6: after release, a plain press+release on the second tab is a
     * select (drag state must not leak across gestures). Second tab spans
     * roughly x in [103, 199]: press and release inside at (150, 18). */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    in.cursor = (flux_point){150, 18};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    test_end(ui);
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_SELECT);
    CHECK(a.index == 1);
    test_end(ui);

    /* Frame 7-8: press on the last tab, drag left to the first, release →
     * MOVE(2 -> 0). Third tab sits right of the second; press (250, 18),
     * release over the first tab at (10, 18). */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    in.cursor = (flux_point){250, 18};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    test_end(ui);
    in.cursor = (flux_point){10, 18};
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_NONE); /* threshold crossed mid-drag */
    test_end(ui);
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_MOVE);
    CHECK(a.index == 2);
    CHECK(a.to == 0);
    test_end(ui);

    /* Model-drift guard: drag armed, then tab_count changes → no MOVE. */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    in.cursor = (flux_point){10, 18};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    test_end(ui);
    in.cursor = (flux_point){300, 18}; /* past threshold */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    test_end(ui);
    /* Model changed under the drag (a tab closed elsewhere). */
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    {
        lens_tab_item tabs[4] = {{.title = "Alpha"},
                                 {.title = "Beta"},
                                 {.title = "Gamma"},
                                 {.title = "Delta"}};
        lens_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = lens_tab_strip(ui, "##main-tabs", tabs, 4, 0, &opts);
    }
    CHECK(a.kind == LENS_TAB_ACTION_NONE);
    test_end(ui);

    lens_release(ui);
    return TEST_REPORT();
}
