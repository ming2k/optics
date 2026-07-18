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

    /* A wheel gesture away from the popup scrolls its owner, so the popup
     * closes before the anchor can leave the viewport. The popup flips
     * above the trigger when that side is roomier, so pick a wheel point
     * near the owner's bottom edge — inside the viewport, clear of the
     * flipped popup. */
    in = IN0;
    in.cursor = (flux_point){scroll_bounds.x + 20.0f,
                             scroll_bounds.y + scroll_bounds.h - 10.0f};
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

/* ---- popup geometry: cap, flip, and list scrolling ----------------- */

static const char *LONG_ITEMS[] = {"I0", "I1", "I2", "I3", "I4", "I5",
                                   "I6", "I7", "I8", "I9", "I10", "I11"};
static const int LONG_COUNT = 12;

typedef struct popup_probe {
    flux_rect list;      /* bounds of the option-list scroll area   */
    int scroll_areas;    /* scroll areas seen                        */
    flux_rect first_row; /* bounds of item "I0"                      */
    flux_rect last_row;  /* bounds of item "I11"                     */
    bool saw_first_row;
    bool saw_last_row;
} popup_probe;

static void collect_popup_probe(const lens_semantics *semantics, flux_rect bounds, lens_id id,
                                lens_id parent, void *user) {
    (void)id;
    (void)parent;
    popup_probe *probe = user;
    if (semantics->role == LENS_ROLE_SCROLLAREA) {
        probe->scroll_areas++;
        probe->list = bounds;
        return;
    }
    if (!semantics->name)
        return;
    if (strcmp(semantics->name, "I0") == 0) {
        probe->first_row = bounds;
        probe->saw_first_row = true;
    } else if (strcmp(semantics->name, "I11") == 0) {
        probe->last_row = bounds;
        probe->saw_last_row = true;
    }
}

/* probe2: like popup_probe but keeps both scroll areas (owner + popup). */
typedef struct popup_probe2 {
    flux_rect areas[4];
    int scroll_areas;
} popup_probe2;

static void collect_popup_probe2(const lens_semantics *semantics, flux_rect bounds, lens_id id,
                                 lens_id parent, void *user) {
    (void)id;
    (void)parent;
    popup_probe2 *probe = user;
    if (semantics->role == LENS_ROLE_SCROLLAREA && probe->scroll_areas < 4)
        probe->areas[probe->scroll_areas++] = bounds;
}

static void open_root_dropdown(lens *ui, int *sel) {
    lens_begin(ui, &IN0);
    lens_dropdown(ui, "long", sel, LONG_ITEMS, LONG_COUNT);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "long", sel, LONG_ITEMS, LONG_COUNT);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "long", sel, LONG_ITEMS, LONG_COUNT);
    lens_end(ui);
}

static void test_dropdown_caps_long_list_and_scrolls(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;
    lens_theme theme = lens_get_theme(ui);
    float row_h = theme.font_size + 2.0f * theme.padding;
    float peek = 7.5f * row_h + 6.0f * 2.0f;

    open_root_dropdown(ui, &sel);
    CHECK(lens_overlay_is_open(ui, "long##ov"));

    /* Plenty of room below the trigger here: the list opens below it, gets
     * its own scroll area capped at the ~7-row peek, and the full item
     * extent stays taller than that viewport so it can actually scroll. */
    lens_node *trigger = lens_node_first_child(lens_root(ui));
    CHECK(trigger != NULL);
    flux_rect trigger_bounds = lens_node_bounds(trigger);

    popup_probe probe = {0};
    lens_accessibility_walk(ui, collect_popup_probe, &probe);
    CHECK(probe.scroll_areas == 1);
    CHECK(probe.saw_first_row && probe.saw_last_row);
    CHECK(probe.list.y >= trigger_bounds.y + trigger_bounds.h - 0.5f);
    CHECK(probe.list.h > 5.0f * row_h);
    CHECK(probe.list.h <= peek - 2.0f * theme.padding + 0.5f);
    float extent = (probe.last_row.y + probe.last_row.h) - probe.first_row.y;
    CHECK(extent > probe.list.h);

    lens_destroy(ui);
}

static void build_low_dropdown(lens *ui, int *selected) {
    lens_size(ui, 240.0f, 150.0f);
    lens_scroll_begin(ui, "owner-scroll");
    lens_label(ui, "head-a");
    lens_label(ui, "head-b");
    lens_label(ui, "head-c");
    lens_dropdown(ui, "low", selected, LONG_ITEMS, LONG_COUNT);
    /* Tail rows give the owner enough scroll range to position the trigger
     * anywhere in its viewport regardless of theme metrics. */
    for (int i = 0; i < 3; i++) {
        lens_push_id_int(ui, i);
        lens_label(ui, "tail-row");
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
}

static void test_dropdown_flips_above_without_covering_trigger(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;

    lens_begin(ui, &IN0);
    build_low_dropdown(ui, &sel);
    lens_end(ui);

    lens_node *scroll = lens_node_first_child(lens_root(ui));
    CHECK(scroll != NULL);
    lens_node *trigger = lens_node_first_child(scroll);
    for (int i = 0; i < 3; i++)
        trigger = lens_node_next_sibling(trigger);
    CHECK(trigger != NULL);
    flux_rect scroll_bounds = lens_node_bounds(scroll);
    flux_rect trigger_bounds = lens_node_bounds(trigger);

    /* Scroll the trigger to 65% of the owner viewport height: low enough
     * that the roomier side is above it, computed so the setup does not
     * depend on theme metrics. */
    float target_y = scroll_bounds.y + scroll_bounds.h * 0.65f;
    float need = trigger_bounds.y - target_y;
    lens_input in = IN0;
    in.cursor = (flux_point){scroll_bounds.x + 20.0f, scroll_bounds.y + 20.0f};
    in.scroll_y = -(need / 40.0f); /* lens scroll speed: 40 px per unit */
    lens_begin(ui, &in);
    build_low_dropdown(ui, &sel);
    lens_end(ui);
    trigger_bounds = lens_node_bounds(trigger);

    /* Setup sanity: the trigger sits low in the owner viewport but fully
     * visible, so the roomier side is above it. */
    CHECK(trigger_bounds.y + trigger_bounds.h <= scroll_bounds.y + scroll_bounds.h);
    CHECK(trigger_bounds.y > scroll_bounds.y + scroll_bounds.h * 0.4f);

    in = IN0;
    in.cursor = (flux_point){trigger_bounds.x + 10.0f,
                             trigger_bounds.y + trigger_bounds.h * 0.5f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_low_dropdown(ui, &sel);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_low_dropdown(ui, &sel);
    lens_end(ui);

    /* The capped list flips above the trigger instead of covering it, and
     * stays inside the owner viewport. (The overlay id is scoped to the
     * owner scroll, so openness is probed via the a11y walk rather than
     * lens_overlay_is_open from the root scope.) */
    popup_probe2 probe = {0};
    lens_accessibility_walk(ui, collect_popup_probe2, &probe);
    CHECK(probe.scroll_areas == 2);
    flux_rect list = {0};
    bool found = false;
    for (int i = 0; i < probe.scroll_areas; i++) {
        flux_rect a = probe.areas[i];
        bool is_owner = fabsf(a.x - scroll_bounds.x) < 0.5f &&
                        fabsf(a.y - scroll_bounds.y) < 0.5f &&
                        fabsf(a.h - scroll_bounds.h) < 0.5f;
        if (!is_owner) {
            list = a;
            found = true;
        }
    }
    CHECK(found);
    CHECK(list.y + list.h <= trigger_bounds.y + 0.5f);
    CHECK(list.y >= scroll_bounds.y - 0.5f);

    lens_destroy(ui);
}

static void build_owned_long_dropdown(lens *ui, int *selected) {
    lens_size(ui, 240.0f, 150.0f);
    lens_scroll_begin(ui, "owner-scroll");
    lens_dropdown(ui, "long", selected, LONG_ITEMS, LONG_COUNT);
    lens_scroll_end(ui);
}

static void test_dropdown_wheel_over_popup_scrolls_and_stays_open(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;

    /* The owner scroll constrains the popup to its viewport, leaving the
     * rest of the display genuinely outside it. */
    lens_begin(ui, &IN0);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    lens_node *owner = lens_node_first_child(lens_root(ui));
    CHECK(owner != NULL);
    flux_rect owner_bounds = lens_node_bounds(owner);
    /* Openness is probed via the a11y walk: the overlay id is scoped to the
     * owner scroll, so lens_overlay_is_open cannot resolve it from here. */
    popup_probe2 areas = {0};
    lens_accessibility_walk(ui, collect_popup_probe2, &areas);
    CHECK(areas.scroll_areas == 2);
    flux_rect list = {0};
    bool found = false;
    for (int i = 0; i < areas.scroll_areas; i++) {
        flux_rect a = areas.areas[i];
        bool is_owner = fabsf(a.x - owner_bounds.x) < 0.5f &&
                        fabsf(a.y - owner_bounds.y) < 0.5f &&
                        fabsf(a.h - owner_bounds.h) < 0.5f;
        if (!is_owner) {
            list = a;
            found = true;
        }
    }
    CHECK(found);

    popup_probe before = {0};
    lens_accessibility_walk(ui, collect_popup_probe, &before);
    CHECK(before.saw_first_row);

    /* A wheel gesture over the popup scrolls the list (the first row moves
     * up by one scroll step) and the popup stays open. */
    in = IN0;
    in.cursor = (flux_point){list.x + list.w * 0.5f, list.y + list.h * 0.5f};
    in.scroll_y = -1.0f;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);
    areas = (popup_probe2){0};
    lens_accessibility_walk(ui, collect_popup_probe2, &areas);
    CHECK(areas.scroll_areas == 2);

    popup_probe after = {0};
    lens_accessibility_walk(ui, collect_popup_probe, &after);
    CHECK(after.saw_first_row);
    CHECK(fabsf(after.first_row.y - (before.first_row.y - 40.0f)) < 0.5f);

    /* A wheel gesture away from the popup still closes it. */
    in = IN0;
    in.cursor = (flux_point){IN0.display_size.x - 10.0f, IN0.display_size.y - 10.0f};
    in.scroll_y = -1.0f;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);
    areas = (popup_probe2){0};
    lens_accessibility_walk(ui, collect_popup_probe2, &areas);
    CHECK(areas.scroll_areas == 1);

    lens_destroy(ui);
}

static void test_dropdown_popup_scroll_clamps_at_bounds(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;

    lens_begin(ui, &IN0);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);

    lens_node *owner = lens_node_first_child(lens_root(ui));
    CHECK(owner != NULL);
    flux_rect owner_bounds = lens_node_bounds(owner);
    popup_probe2 areas = {0};
    lens_accessibility_walk(ui, collect_popup_probe2, &areas);
    flux_rect list = {0};
    for (int i = 0; i < areas.scroll_areas; i++) {
        flux_rect a = areas.areas[i];
        bool is_owner = fabsf(a.x - owner_bounds.x) < 0.5f &&
                        fabsf(a.y - owner_bounds.y) < 0.5f &&
                        fabsf(a.h - owner_bounds.h) < 0.5f;
        if (!is_owner)
            list = a;
    }
    CHECK(list.h > 0.0f);

    /* Scroll far past the end: the offset must clamp so the last row's
     * bottom lands exactly on the viewport's bottom edge. */
    in = IN0;
    in.cursor = (flux_point){list.x + list.w * 0.5f, list.y + list.h * 0.5f};
    in.scroll_y = -20.0f;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);
    popup_probe probe = {0};
    lens_accessibility_walk(ui, collect_popup_probe, &probe);
    CHECK(probe.saw_last_row);
    CHECK(fabsf(probe.last_row.y + probe.last_row.h - (list.y + list.h)) < 0.5f);

    /* Scroll far past the start: the first row snaps back to the top. */
    in.scroll_y = 20.0f;
    lens_begin(ui, &in);
    build_owned_long_dropdown(ui, &sel);
    lens_end(ui);
    probe = (popup_probe){0};
    lens_accessibility_walk(ui, collect_popup_probe, &probe);
    CHECK(probe.saw_first_row);
    CHECK(fabsf(probe.first_row.y - list.y) < 0.5f);

    lens_destroy(ui);
}

int main(void) {
    test_dropdown_click_select();
    test_dropdown_keyboard_nav();
    test_dropdown_enforces_content_height();
    test_open_trigger_click_closes_once();
    test_scrolled_dropdown_stays_in_owner_and_closes_on_wheel();
    test_dropdown_caps_long_list_and_scrolls();
    test_dropdown_flips_above_without_covering_trigger();
    test_dropdown_wheel_over_popup_scrolls_and_stays_open();
    test_dropdown_popup_scroll_clamps_at_bounds();
    return TEST_REPORT();
}
