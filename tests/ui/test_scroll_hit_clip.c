/* test_scroll_hit_clip.c — children scrolled out of a scroll viewport
 * must not be hoverable/clickable: the render clips them, hit-testing
 * must clip identically. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static const char *ROW_LABELS[20] = {"row-0",  "row-1",  "row-2",  "row-3",  "row-4",
                                     "row-5",  "row-6",  "row-7",  "row-8",  "row-9",
                                     "row-10", "row-11", "row-12", "row-13", "row-14",
                                     "row-15", "row-16", "row-17", "row-18", "row-19"};

/* A button beyond the scroll viewport's bottom edge is invisible; input
 * landing on its arranged rect must not reach it. After scrolling down
 * it becomes visible at that spot and works. */
static void test_scroll_clips_hit_testing(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool clicked = false;
    bool hovered = false;

    /* Frame 1: establish geometry (viewport ~60px, 20 buttons). */
    optics_ui_begin(ui, &IN0);
    optics_ui_size(ui, 200.0f, 60.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
    for (int i = 0; i < 20; ++i) {
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]}).clicked)
            clicked = true;
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* Frame 2: press+release at y=200 — inside a folded button's rect,
     * far below the viewport. */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){30.0f, 200.0f};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200.0f, 60.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
    for (int i = 0; i < 20; ++i) {
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]}).clicked) {
            clicked = true;
        }
        if (i == 0)
            hovered = optics_ui_get_response(ui).hovered;
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);
    in = IN0;
    in.cursor = (optics_gfx_point){30.0f, 200.0f};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200.0f, 60.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
    for (int i = 0; i < 20; ++i) {
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]}).clicked)
            clicked = true;
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);
    (void)hovered;

    /* Scroll to the bottom, then the same point hits the visible row. */
    for (int frame = 0; frame < 3; ++frame) {
        in = IN0;
        in.cursor = (optics_gfx_point){30.0f, 30.0f};
        in.scroll_y = -5.0f;
        optics_ui_begin(ui, &in);
        optics_ui_size(ui, 200.0f, 60.0f);
        optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
        for (int i = 0; i < 20; ++i)
            (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]});
        optics_ui_scroll_end(ui);
        optics_ui_test_end(ui);
    }
    in = IN0;
    in.cursor = (optics_gfx_point){30.0f, 30.0f};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200.0f, 60.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
    for (int i = 0; i < 20; ++i)
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]});
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);
    in = IN0;
    in.cursor = (optics_gfx_point){30.0f, 30.0f};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200.0f, 60.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "list"}});
    for (int i = 0; i < 20; ++i) {
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = ROW_LABELS[i]}).clicked)
            clicked = true;
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

int main(void) {
    test_scroll_clips_hit_testing();
    return OT_REPORT();
}
