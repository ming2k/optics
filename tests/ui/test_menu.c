/* test_menu.c — verification for cascading menu and menubar pattern. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui/patterns.h>

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(nullptr, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    optics_ui_menubar_item mb_items[2] = {
        {.title = "File", .menu_id = "file_menu"},
        {.title = "Edit", .menu_id = "edit_menu"},
    };

    optics_ui_menu_item file_items[4] = {
        {.label = "New", .shortcut = "Ctrl+N"},
        {.label = "Open...", .shortcut = "Ctrl+O"},
        {.is_separator = true},
        {.label = "Exit", .disabled = true},
    };

    optics_ui_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};
    optics_gfx_rect anchor = {};

    optics_ui_menubar_opts mb_opts = {
        .id = "mb",
        .items = mb_items,
        .item_count = 2,
        .out_anchor = &anchor,
    };

    /* Frame 1: Neutral pass to layout menubar */
    optics_ui_begin(ui, &in);
    int32_t open_menu = optics_ui_menubar(ui, &mb_opts);
    OT_EXPECT(open_menu == -1);
    optics_ui_test_end(ui);

    /* Frame 2: Click on "File" button (x ~ 15, y ~ 10) */
    in.cursor = (optics_gfx_point){15.0f, 10.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_menubar(ui, &mb_opts);
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    open_menu = optics_ui_menubar(ui, &mb_opts);
    OT_EXPECT(open_menu == 0);
    OT_EXPECT(anchor.w > 0.0f);

    /* Render popup menu for "file_menu" anchored to the File button */
    int32_t act = optics_ui_popup_menu(
        ui, &(optics_ui_menu_opts){
                .id = "file_menu", .anchor = anchor, .items = file_items, .item_count = 4});
    OT_EXPECT(act == -1); /* None clicked yet */
    optics_ui_test_end(ui);

    /* Frame 3: Click on "Open..." inside popup menu (item 1, y ~ anchor.y + anchor.h + 28 + 14) */
    in.cursor = (optics_gfx_point){anchor.x + 40.0f, anchor.y + anchor.h + 38.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    optics_ui_menubar(ui, &mb_opts);
    optics_ui_popup_menu(
        ui, &(optics_ui_menu_opts){
                .id = "file_menu", .anchor = anchor, .items = file_items, .item_count = 4});
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_menubar(ui, &mb_opts);
    act = optics_ui_popup_menu(
        ui, &(optics_ui_menu_opts){
                .id = "file_menu", .anchor = anchor, .items = file_items, .item_count = 4});
    OT_EXPECT(act == 1); /* "Open..." clicked! */
    optics_ui_test_end(ui);

    /* Frame 4: Menu should now be closed */
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    open_menu = optics_ui_menubar(ui, &mb_opts);
    OT_EXPECT(open_menu == -1);
    optics_ui_test_end(ui);

    /* Frame 5: Re-open menu programmatically and navigate via keyboard */
    optics_ui_place_open(ui, "file_menu");
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_DOWN, .pressed = true};
    optics_ui_begin(ui, &in);
    act = optics_ui_popup_menu(
        ui, &(optics_ui_menu_opts){
                .id = "file_menu", .anchor = anchor, .items = file_items, .item_count = 4});
    OT_EXPECT(act == -1);
    optics_ui_test_end(ui);

    /* Press Return -> activates item 0 ("New") */
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    act = optics_ui_popup_menu(
        ui, &(optics_ui_menu_opts){
                .id = "file_menu", .anchor = anchor, .items = file_items, .item_count = 4});
    OT_EXPECT(act == 0); /* "New" activated via keyboard Return */
    optics_ui_test_end(ui);

    optics_ui_release(ui);
    return OT_REPORT();
}
