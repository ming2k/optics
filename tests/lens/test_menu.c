/* test_menu.c — menu bar, items, context menu, and submenu (ADR-0017). */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* A menu bar with one menu builds without error; the body does not run
 * until the menu is opened. */
static void test_menubar_builds_closed(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int items_run = 0;
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item(ui, "New", "Ctrl-N"))
            items_run++;
        if (lens_menu_item(ui, "Open", "Ctrl-O"))
            items_run++;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(items_run == 0); /* menu closed → body skipped */

    lens_destroy(ui);
}

/* Clicking a menu bar trigger opens its overlay so the body runs.
 * A click is a press frame then a release frame; the body runs on the
 * release frame when r.clicked fires and opens the overlay in-frame. */
static void test_click_opens_menu(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: build so the trigger has a prev_rect. */
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    /* Frame 2: press the "File" trigger (sets active_id). */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){20, 10};
    pin.mouse_pressed[LENS_MOUSE_LEFT] = true;
    pin.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &pin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    /* Frame 3: release → r.clicked → overlay opens, body runs this frame. */
    lens_input rin = ZERO_IN;
    rin.cursor = (flux_point){20, 10};
    rin.mouse_down[LENS_MOUSE_LEFT] = true;
    rin.mouse_released[LENS_MOUSE_LEFT] = true;
    int body_run = 0;
    lens_begin(ui, &rin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        body_run++;
        lens_menu_item(ui, "New", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(body_run == 1); /* opened */

    lens_destroy(ui);
}

/* Clicking a menu item returns true and dismisses the whole stack.
 * Sequence: open trigger (press+release), settle, then press+release
 * on the item. */
static void test_item_click_fires_and_closes(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: build the trigger. */
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    /* Frame 2: press trigger; frame 3: release → opens the menu. */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){20, 10};
    pin.mouse_pressed[LENS_MOUSE_LEFT] = true;
    pin.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &pin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    lens_input rin = ZERO_IN;
    rin.cursor = (flux_point){20, 10};
    rin.mouse_down[LENS_MOUSE_LEFT] = true;
    rin.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &rin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    /* Frame 4: settle so item prev_rects are valid; discover item bounds. */
    flux_rect new_rect = {0};
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        (void)lens_menu_item(ui, "New", NULL);
        new_rect = lens_get_response(ui).rect;
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(new_rect.w > 0); /* menu is open and the item has geometry */

    float hx = new_rect.x + new_rect.w * 0.5f;
    float hy = new_rect.y + new_rect.h * 0.5f;

    /* Frame 5: press the first item; frame 6: release → fires + closes. */
    lens_input ipin = ZERO_IN;
    ipin.cursor = (flux_point){hx, hy};
    ipin.mouse_pressed[LENS_MOUSE_LEFT] = true;
    ipin.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &ipin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        lens_menu_item(ui, "New", NULL);
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    bool fired = false;
    lens_input irin = ZERO_IN;
    irin.cursor = (flux_point){hx, hy};
    irin.mouse_down[LENS_MOUSE_LEFT] = true;
    irin.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &irin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item(ui, "New", NULL))
            fired = true;
        lens_menu_item(ui, "Open", NULL);
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(fired == true);

    /* Next frame: menu body should NOT run (stack dismissed). */
    int body_run = 0;
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        body_run++;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(body_run == 0);

    lens_destroy(ui);
}

/* Disabled items never fire and render dimmed. */
static void test_disabled_item_does_not_fire(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Open menu, settle. */
    for (int s = 0; s < 2; s++) {
        lens_input in = ZERO_IN;
        in.cursor = (flux_point){20, 10};
        if (s == 0) {
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
            in.mouse_down[LENS_MOUSE_LEFT] = true;
        }
        lens_begin(ui, &in);
        lens_menubar_begin(ui, "mb");
        if (lens_menu_begin(ui, "File")) {
            lens_menu_item_disabled(ui, "Save", "Ctrl-S");
            lens_menu_end(ui);
        }
        lens_menubar_end(ui);
        lens_end(ui);
    }

    /* Click the disabled item. Discover its rect first. */
    flux_rect save_rect = {0};
    lens_begin(ui, &ZERO_IN);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        (void)lens_menu_item_disabled(ui, "Save", "Ctrl-S");
        save_rect = lens_get_response(ui).rect;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);

    bool fired = false;
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){save_rect.x + save_rect.w * 0.5f, save_rect.y + save_rect.h * 0.5f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item_disabled(ui, "Save", "Ctrl-S"))
            fired = true;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    /* release frame too — a disabled item still must not fire on release */
    lens_input rin = ZERO_IN;
    rin.cursor = (flux_point){save_rect.x + save_rect.w * 0.5f, save_rect.y + save_rect.h * 0.5f};
    rin.mouse_down[LENS_MOUSE_LEFT] = true;
    rin.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &rin);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item_disabled(ui, "Save", "Ctrl-S"))
            fired = true;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(fired == false);

    lens_destroy(ui);
}

/* Context menu opens at the cursor on right-click and builds its body.
 * right_clicked fires on release, so press then release the right button. */
static void test_context_menu(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: build the base widget so it has a prev_rect. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_menu_item(ui, "Copy", NULL);
        lens_context_menu_end(ui);
    }
    lens_end(ui);

    /* Frame 2: press right button inside the area. */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){20, 8};
    pin.mouse_down[LENS_MOUSE_RIGHT] = true;
    pin.mouse_pressed[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &pin);
    (void)lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_menu_item(ui, "Copy", "Ctrl-C");
        lens_context_menu_end(ui);
    }
    lens_end(ui);

    /* Frame 3: release right button → right_clicked → open → body runs. */
    lens_input rin = ZERO_IN;
    rin.cursor = (flux_point){20, 8};
    rin.mouse_down[LENS_MOUSE_RIGHT] = true;
    rin.mouse_released[LENS_MOUSE_RIGHT] = true;
    int body_run = 0;
    lens_begin(ui, &rin);
    lens_response r = lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    if (r.right_clicked)
        lens_context_menu_open(ui, "ctx", r.rect);
    if (lens_context_menu_begin(ui, "ctx")) {
        body_run++;
        lens_menu_item(ui, "Copy", "Ctrl-C");
        lens_context_menu_end(ui);
    }
    lens_end(ui);
    CHECK(body_run == 1);

    lens_destroy(ui);
}

/* Escape closes an open context menu (overlay dismissal). */
static void test_escape_closes_context_menu(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Build + settle the base widget. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    lens_end(ui);

    /* Press + release right button to open the context menu. */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){20, 8};
    pin.mouse_down[LENS_MOUSE_RIGHT] = true;
    pin.mouse_pressed[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &pin);
    (void)lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_menu_item(ui, "Copy", NULL);
        lens_context_menu_end(ui);
    }
    lens_end(ui);

    lens_input oin = ZERO_IN;
    oin.cursor = (flux_point){20, 8};
    oin.mouse_down[LENS_MOUSE_RIGHT] = true;
    oin.mouse_released[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &oin);
    lens_response r = lens_button_ex(ui, (lens_button_opts){.label = "Area"});
    if (r.right_clicked)
        lens_context_menu_open(ui, "ctx", r.rect);
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_menu_item(ui, "Copy", NULL);
        lens_context_menu_end(ui);
    }
    lens_end(ui);

    /* Escape closes it — but dismissal runs at lens_end, so the body still
     * runs on the Escape frame itself. Verify it is gone the frame after. */
    lens_input ein = ZERO_IN;
    ein.key_count = 1;
    ein.keys[0] = (lens_key_event){.key = LENS_KEY_ESCAPE, .pressed = true};
    lens_begin(ui, &ein);
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_context_menu_end(ui);
    }
    lens_end(ui);

    int body_run = 0;
    lens_begin(ui, &ZERO_IN);
    if (lens_context_menu_begin(ui, "ctx")) {
        body_run++;
        lens_context_menu_end(ui);
    }
    lens_end(ui);
    CHECK(body_run == 0);

    lens_destroy(ui);
}

int main(void) {
    test_menubar_builds_closed();
    test_click_opens_menu();
    test_item_click_fires_and_closes();
    test_disabled_item_does_not_fire();
    test_context_menu();
    test_escape_closes_context_menu();
    return TEST_REPORT();
}
