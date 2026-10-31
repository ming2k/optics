/* test_menu.c — verification for cascading menu and menubar pattern. */

#include "test_helpers.h"
#include <lens/patterns.h>

int main(void) {
    lens *ui = nullptr;
    CHECK(lens_create(nullptr, &ui) == FLUX_OK);
    CHECK(ui != nullptr);

    lens_menubar_item mb_items[2] = {
        {.title = "File", .menu_id = "file_menu"},
        {.title = "Edit", .menu_id = "edit_menu"},
    };

    lens_menu_item file_items[4] = {
        {.label = "New", .shortcut = "Ctrl+N"},
        {.label = "Open...", .shortcut = "Ctrl+O"},
        {.is_separator = true},
        {.label = "Exit", .disabled = true},
    };

    lens_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};
    flux_rect anchor = {0};

    /* Frame 1: Neutral pass to layout menubar */
    lens_begin(ui, &in);
    int32_t open_menu = lens_menubar(ui, "mb", mb_items, 2, &anchor);
    CHECK(open_menu == -1);
    test_end(ui);

    /* Frame 2: Click on "File" button (x ~ 15, y ~ 10) */
    in.cursor = (flux_point){15.0f, 10.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_menubar(ui, "mb", mb_items, 2, &anchor);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    open_menu = lens_menubar(ui, "mb", mb_items, 2, &anchor);
    CHECK(open_menu == 0);
    CHECK(anchor.w > 0.0f);

    /* Render popup menu for "file_menu" anchored to the File button */
    int32_t act = lens_popup_menu(ui, "file_menu", anchor, file_items, 4, nullptr);
    CHECK(act == -1); /* None clicked yet */
    test_end(ui);

    /* Frame 3: Click on "Open..." inside popup menu (item 1, y ~ anchor.y + anchor.h + 28 + 14) */
    in.cursor = (flux_point){anchor.x + 40.0f, anchor.y + anchor.h + 38.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    lens_menubar(ui, "mb", mb_items, 2, &anchor);
    lens_popup_menu(ui, "file_menu", anchor, file_items, 4, nullptr);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_menubar(ui, "mb", mb_items, 2, &anchor);
    act = lens_popup_menu(ui, "file_menu", anchor, file_items, 4, nullptr);
    CHECK(act == 1); /* "Open..." clicked! */
    test_end(ui);

    /* Frame 4: Menu should now be closed */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    open_menu = lens_menubar(ui, "mb", mb_items, 2, &anchor);
    CHECK(open_menu == -1);
    test_end(ui);

    /* Frame 5: Re-open menu programmatically and navigate via keyboard */
    lens_place_open(ui, "file_menu");
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    lens_begin(ui, &in);
    act = lens_popup_menu(ui, "file_menu", anchor, file_items, 4, nullptr);
    CHECK(act == -1);
    test_end(ui);

    /* Press Return -> activates item 0 ("New") */
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    act = lens_popup_menu(ui, "file_menu", anchor, file_items, 4, nullptr);
    CHECK(act == 0); /* "New" activated via keyboard Return */
    test_end(ui);

    lens_release(ui);
    return TEST_REPORT();
}
