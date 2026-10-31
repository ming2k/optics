/* test_tree_view.c — verification for hierarchical tree view pattern. */

#include "test_helpers.h"
#include <lens/patterns.h>

int main(void) {
    lens *ui = nullptr;
    CHECK(lens_create(nullptr, &ui) == FLUX_OK);
    CHECK(ui != nullptr);

    lens_tree_item items[4] = {
        {.label = "Project", .depth = 0, .is_leaf = false, .expanded = false},
        {.label = "src", .depth = 1, .is_leaf = false, .expanded = false},
        {.label = "main.c", .depth = 2, .is_leaf = true, .expanded = false, .badge = "C"},
        {.label = "README.md", .depth = 1, .is_leaf = true, .expanded = false, .badge = "MD"},
    };

    lens_tree_opts opts = {
        .row_height = 26.0f,
        .indent_per_level = 16.0f,
    };

    lens_input in = {.display_size = {600, 400}, .dt_seconds = 0.016f};

    /* Frame 1: Neutral pass to layout rows */
    lens_begin(ui, &in);
    lens_tree_action a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    CHECK(a.kind == LENS_TREE_ACTION_NONE);
    test_end(ui);

    /* Frame 2: Click on chevron of item 0 (x ~ 10, y ~ 13) */
    in.cursor = (flux_point){10.0f, 13.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    CHECK(a.kind == LENS_TREE_ACTION_TOGGLE);
    CHECK(a.index == 0);
    CHECK(items[0].expanded == true);
    test_end(ui);

    /* Frame 3: Click on row body of item 3 (x ~ 60, y ~ 26*3 + 13 = 91) */
    in.cursor = (flux_point){60.0f, 91.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    CHECK(a.kind == LENS_TREE_ACTION_SELECT);
    CHECK(a.index == 3);
    test_end(ui);

    /* Frame 4: Keyboard navigation test */
    /* Press Down arrow from item 3 (already at bottom) -> stays or can test Up */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_UP, .pressed = true};
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    CHECK(a.kind == LENS_TREE_ACTION_SELECT);
    CHECK(a.index == 2);
    test_end(ui);

    /* Press Return -> activates item 2 */
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    a = lens_tree_view(ui, "my_tree", items, 4, &opts);
    CHECK(a.kind == LENS_TREE_ACTION_ACTIVATE);
    CHECK(a.index == 2);
    test_end(ui);

    lens_release(ui);
    return TEST_REPORT();
}
