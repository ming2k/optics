/* test_tree_view.c — verification for hierarchical tree view pattern. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui/patterns.h>

/* The fixed arguments of every case in this file, merged into a copy of the
 * case's options. */
static optics_ui_tree_action tree_view(optics_ui *ui, const char *id, optics_ui_tree_item *items,
                                       uint32_t item_count, const optics_ui_tree_opts *base) {
    optics_ui_tree_opts o = *base;
    o.id = id;
    o.items = items;
    o.item_count = item_count;
    return optics_ui_tree_view(ui, &o);
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(nullptr, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    optics_ui_tree_item items[4] = {
        {.label = "Project", .depth = 0, .is_leaf = false, .expanded = false},
        {.label = "src", .depth = 1, .is_leaf = false, .expanded = false},
        {.label = "main.c", .depth = 2, .is_leaf = true, .expanded = false, .badge = "C"},
        {.label = "README.md", .depth = 1, .is_leaf = true, .expanded = false, .badge = "MD"},
    };

    optics_ui_tree_opts opts = {
        .row_height = 26.0f,
        .indent_per_level = 16.0f,
    };

    optics_ui_input in = {.display_size = {600, 400}, .dt_seconds = 0.016f};

    /* Frame 1: Neutral pass to layout rows */
    optics_ui_begin(ui, &in);
    optics_ui_tree_action a = tree_view(ui, "my_tree", items, 4, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_TREE_ACTION_NONE);
    optics_ui_test_end(ui);

    /* Frame 2: Click on chevron of item 0 (x ~ 10, y ~ 13) */
    in.cursor = (optics_gfx_point){10.0f, 13.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_TREE_ACTION_TOGGLE);
    OT_EXPECT(a.index == 0);
    OT_EXPECT(items[0].expanded == true);
    optics_ui_test_end(ui);

    /* Frame 3: Click on row body of item 3 (x ~ 60, y ~ 26*3 + 13 = 91) */
    in.cursor = (optics_gfx_point){60.0f, 91.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_TREE_ACTION_SELECT);
    OT_EXPECT(a.index == 3);
    optics_ui_test_end(ui);

    /* Frame 4: Keyboard navigation test */
    /* Press Down arrow from item 3 (already at bottom) -> stays or can test Up */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_UP, .pressed = true};
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_TREE_ACTION_SELECT);
    OT_EXPECT(a.index == 2);
    optics_ui_test_end(ui);

    /* Press Return -> activates item 2 */
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    a = tree_view(ui, "my_tree", items, 4, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_TREE_ACTION_ACTIVATE);
    OT_EXPECT(a.index == 2);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
    return OT_REPORT();
}
