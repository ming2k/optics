/* test_tab_strip.c — tab strip drag-to-reorder verification. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui/icon.h>
#include <optics/ui/patterns.h>
#include <stdio.h>
#include <string.h>

/* The fixed arguments of every case in this file, merged into a copy of the
 * case's options. */
static optics_ui_tab_action tab_strip(optics_ui *ui, const char *id, const optics_ui_tab_item *tabs,
                                      uint32_t tab_count, uint32_t active_index,
                                      const optics_ui_tab_strip_opts *base) {
    optics_ui_tab_strip_opts o = *base;
    o.id = id;
    o.tabs = tabs;
    o.tab_count = tab_count;
    o.active_index = active_index;
    return optics_ui_tab_strip(ui, &o);
}

/* Build a three-tab strip, returns nothing; helpers share one layout fn so
 * ids stay stable across frames. */
static void build_strip(optics_ui *ui, uint32_t active) {
    optics_ui_tab_item tabs[3] = {
        {.title = "Alpha", .icon = OPTICS_UI_ICON_INVALID, .closable = true},
        {.title = "Beta", .icon = OPTICS_UI_ICON_INVALID, .closable = true},
        {.title = "Gamma", .icon = OPTICS_UI_ICON_INVALID, .closable = true},
    };
    optics_ui_tab_strip_opts opts = {.height = 36.0f,
                                     .min_tab_width = 80.0f,
                                     .max_tab_width = 120.0f,
                                     .show_new_button = true,
                                     .close_icon = OPTICS_UI_ICON_INVALID,
                                     .new_icon = OPTICS_UI_ICON_INVALID};
    tab_strip(ui, "##main-tabs", tabs, 3, active, &opts);
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(nullptr, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    optics_ui_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: neutral build so tab rects exist for interaction (ADR-0029). */
    in.cursor = (optics_gfx_point){0, 0};
    optics_ui_begin(ui, &in);
    build_strip(ui, 0);
    optics_ui_test_end(ui);

    /* Frame 2: press on the first tab — the strip must flag the press so a
     * CSD host suppresses the window-move grab (tab drags take priority). */
    in.cursor = (optics_gfx_point){10, 18};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_tab_action a = {};
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    /* A press without release is not a select yet. */
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_NONE);
    OT_EXPECT(a.pressed_on_tab);
    optics_ui_test_end(ui);

    /* Frame 3: small movement below the threshold — still no action. */
    in.cursor = (optics_gfx_point){16, 22};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_NONE);
    optics_ui_test_end(ui);

    /* Frame 4: movement past the 14px threshold — drag becomes active but
     * nothing fires until release. Cursor far right: predicted slot = 2
     * (post-removal coordinates). */
    in.cursor = (optics_gfx_point){300, 18};
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_NONE);
    optics_ui_test_end(ui);

    /* Frame 5: release over the third tab region → MOVE(0 -> 2). */
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_MOVE);
    OT_EXPECT(a.index == 0);
    OT_EXPECT(a.to == 2);
    optics_ui_test_end(ui);

    /* Frame 6: after release, a plain press+release on the second tab is a
     * select (drag state must not leak across gestures). Second tab spans
     * roughly x in [103, 199]: press and release inside at (150, 18). */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    in.cursor = (optics_gfx_point){150, 18};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    optics_ui_test_end(ui);
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_SELECT);
    OT_EXPECT(a.index == 1);
    optics_ui_test_end(ui);

    /* Frame 7-8: press on the last tab, drag left to the first, release →
     * MOVE(2 -> 0). Third tab sits right of the second; press (210, 18),
     * release over the first tab at (10, 18). */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    in.cursor = (optics_gfx_point){210, 18};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    optics_ui_test_end(ui);
    in.cursor = (optics_gfx_point){10, 18};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_NONE); /* threshold crossed mid-drag */
    optics_ui_test_end(ui);
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_MOVE);
    OT_EXPECT(a.index == 2);
    OT_EXPECT(a.to == 0);
    optics_ui_test_end(ui);

    /* Model-drift guard: drag armed, then tab_count changes → no MOVE. */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    in.cursor = (optics_gfx_point){10, 18};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    optics_ui_test_end(ui);
    in.cursor = (optics_gfx_point){300, 18}; /* past threshold */
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    optics_ui_test_end(ui);
    /* Model changed under the drag (a tab closed elsewhere). */
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[4] = {
            {.title = "Alpha"}, {.title = "Beta"}, {.title = "Gamma"}, {.title = "Delta"}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 4, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_NONE);
    optics_ui_test_end(ui);

    /* Close button on hover: hover over tab 0's close button (x ≈ 70, y ≈ 18).
     * Press and release triggers OPTICS_UI_TAB_ACTION_CLOSE. */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    in.cursor = (optics_gfx_point){70, 18};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha", .closable = true},
                                      {.title = "Beta", .closable = true},
                                      {.title = "Gamma", .closable = true}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.pressed_on_tab);
    optics_ui_test_end(ui);
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    {
        optics_ui_tab_item tabs[3] = {{.title = "Alpha", .closable = true},
                                      {.title = "Beta", .closable = true},
                                      {.title = "Gamma", .closable = true}};
        optics_ui_tab_strip_opts opts = {.min_tab_width = 80.0f, .max_tab_width = 120.0f};
        a = tab_strip(ui, "##main-tabs", tabs, 3, 0, &opts);
    }
    OT_EXPECT(a.kind == OPTICS_UI_TAB_ACTION_CLOSE);
    OT_EXPECT(a.index == 0);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
    return OT_REPORT();
}
