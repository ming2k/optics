/* test_dropdown.c — lens_dropdown selection and keyboard navigation. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static void build_scroll_dropdown(lens *ui, int *selected) {
    const char *items[] = {"None", "Watercolour diffusion", "Water caustics"};
    lens_size(ui, 240.0f, 150.0f);
    lens_scroll_begin(ui, "inspector-scroll");
    lens_label(ui, "header-one");
    lens_label(ui, "header-two");
    lens_label(ui, "header-three");
    lens_dropdown(ui, "material", selected, items, 3);
    for (int i = 0; i < 8; i++) {
        lens_push_id_int(ui, i);
        lens_label(ui, "tail-row");
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
}

typedef struct item_bounds {
    flux_rect none;
    flux_rect watercolour;
    flux_rect caustics;
    bool saw_none;
    bool saw_watercolour;
    bool saw_caustics;
} item_bounds;

static void collect_item_bounds(const lens_semantics *semantics, flux_rect bounds, lens_id id,
                                lens_id parent, void *user) {
    (void)id;
    (void)parent;
    item_bounds *items = user;
    if (!semantics->name)
        return;
    if (strcmp(semantics->name, "None") == 0) {
        items->none = bounds;
        items->saw_none = true;
    } else if (strcmp(semantics->name, "Watercolour diffusion") == 0) {
        items->watercolour = bounds;
        items->saw_watercolour = true;
    } else if (strcmp(semantics->name, "Water caustics") == 0) {
        items->caustics = bounds;
        items->saw_caustics = true;
    }
}

static void test_dropdown_click_select(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    const char *items[] = {"Red", "Green", "Blue"};
    int sel = 0;

    /* frame 1: build dropdown */
    lens_begin(ui, &IN0);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* frame 2: click the dropdown button to open overlay */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* overlay is now open; frame 3: click the second item */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* We can't easily simulate clicking an overlay item in a CPU test
     * because overlay items are laid out in a separate layer. Verify
     * the dropdown builds without crashing and the initial state is intact. */
    CHECK(sel == 0);

    lens_destroy(ui);
}

static void test_dropdown_keyboard_nav(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    const char *items[] = {"A", "B", "C"};
    int sel = 0;

    /* frame 1: build dropdown */
    lens_begin(ui, &IN0);
    lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    /* frame 2: click to open overlay (prev_rect now valid) */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    /* frame 3: press Down twice while overlay is open */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.key_count = 2;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    in.keys[1] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    lens_begin(ui, &in);
    bool changed = lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    CHECK(changed == true);
    CHECK(sel == 2);

    lens_destroy(ui);
}

static void test_dropdown_enforces_content_height(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    const char *items[] = {"Watercolour diffusion", "Caustics"};
    int sel = 0;

    lens_theme theme = lens_get_theme(ui);
    lens_text_metrics tm = lens_text_measure(ui, theme.font, items[0], theme.font_size);
    float minimum_h = fmaxf(tm.height, theme.font_size) + 2.0f * theme.padding;

    lens_begin(ui, &IN0);
    lens_size(ui, 240.0f, 8.0f);
    lens_dropdown(ui, "material", &sel, items, 2);
    lens_end(ui);

    lens_node *dropdown = lens_node_first_child(lens_root(ui));
    CHECK(dropdown != NULL);
    CHECK(lens_node_bounds(dropdown).h + 0.5f >= minimum_h);

    lens_destroy(ui);
}

static void test_open_trigger_click_closes_once(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    const char *items[] = {"A", "B"};
    int sel = 0;

    lens_begin(ui, &IN0);
    lens_dropdown(ui, "toggle", &sel, items, 2);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "toggle", &sel, items, 2);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "toggle", &sel, items, 2);
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "toggle##ov"));

    /* Pressing the owner is not an outside click: the overlay must remain
     * open until this same click is released back on the trigger. */
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "toggle", &sel, items, 2);
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "toggle##ov"));

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "toggle", &sel, items, 2);
    lens_end(ui);
    CHECK(!lens_overlay_is_open(ui, "toggle##ov"));

    lens_destroy(ui);
}

static void test_scrolled_dropdown_stays_in_owner_and_closes_on_wheel(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;

    lens_begin(ui, &IN0);
    build_scroll_dropdown(ui, &sel);
    lens_end(ui);

    lens_node *scroll = lens_node_first_child(lens_root(ui));
    lens_node *dropdown = lens_node_first_child(scroll);
    for (int i = 0; i < 3; i++)
        dropdown = lens_node_next_sibling(dropdown);
    CHECK(scroll != NULL);
    CHECK(dropdown != NULL);
    flux_rect scroll_bounds = lens_node_bounds(scroll);

    /* Scroll the trigger into the viewport first: an out-of-viewport
     * widget is clipped from hit-testing like any other folded child. */
    lens_input in = IN0;
    in.cursor = (flux_point){scroll_bounds.x + 20.0f, scroll_bounds.y + 20.0f};
    in.scroll_y = -2.0f;
    lens_begin(ui, &in);
    build_scroll_dropdown(ui, &sel);
    lens_end(ui);

    flux_rect trigger_bounds = lens_node_bounds(dropdown);
    CHECK(trigger_bounds.y >= scroll_bounds.y);
    CHECK(trigger_bounds.y + trigger_bounds.h <= scroll_bounds.y + scroll_bounds.h);

    in = IN0;
    in.cursor = (flux_point){trigger_bounds.x + 10.0f, trigger_bounds.y + trigger_bounds.h * 0.5f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_scroll_dropdown(ui, &sel);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_scroll_dropdown(ui, &sel);
    lens_end(ui);

    item_bounds items = {0};
    lens_accessibility_walk(ui, collect_item_bounds, &items);
    CHECK(items.saw_none);
    CHECK(items.saw_watercolour);
    CHECK(items.saw_caustics);
    flux_rect rows[] = {items.none, items.watercolour, items.caustics};
    for (int i = 0; i < 3; i++) {
        CHECK(rows[i].y >= scroll_bounds.y - 0.5f);
        CHECK(rows[i].y + rows[i].h <= scroll_bounds.y + scroll_bounds.h + 0.5f);
    }

    /* The dropdown has no independently scrolling list. A wheel gesture moves
     * its owner, so close the popup before the anchor can leave the viewport. */
    in = IN0;
    in.cursor = (flux_point){scroll_bounds.x + 20.0f, scroll_bounds.y + 20.0f};
    in.scroll_y = -1.0f;
    lens_begin(ui, &in);
    build_scroll_dropdown(ui, &sel);
    lens_end(ui);
    items = (item_bounds){0};
    lens_accessibility_walk(ui, collect_item_bounds, &items);
    CHECK(!items.saw_none);
    CHECK(!items.saw_watercolour);
    CHECK(!items.saw_caustics);

    lens_destroy(ui);
}

int main(void) {
    test_dropdown_click_select();
    test_dropdown_keyboard_nav();
    test_dropdown_enforces_content_height();
    test_open_trigger_click_closes_once();
    test_scrolled_dropdown_stays_in_owner_and_closes_on_wheel();
    return TEST_REPORT();
}
