/* test_split.c — resizable split panel (ADR-0018): build, ratio seed,
 * handle drag, min-size clamp. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* A split with two panes builds and lays out without error. */
static void test_split_builds(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_id split_id = lens_current_id(ui, "s");
    lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL, &(lens_split_opts){.ratio = 0.5f});
    {
        lens_split_pane(ui);
        lens_label(ui, "left");
        lens_split_pane(ui);
        lens_label(ui, "right");
    }
    lens_split_end(ui);
    lens_end(ui);

    CHECK(lens_split_ratio(ui, "s") == 0.5f);
    lens_node *split = lens_find(ui, split_id);
    lens_node *pane1 = lens_node_first_child(split);
    lens_node *pane2 = lens_node_next_sibling(pane1);
    CHECK(split != NULL);
    CHECK(pane1 != NULL);
    CHECK(pane2 != NULL);
    CHECK(lens_node_parent(pane1) == split);
    CHECK(lens_node_parent(pane2) == split);

    lens_destroy(ui);
}

/* Seeding the ratio sizes the first pane accordingly. Verify via the
 * accessibility walk (post-layout bounds). */
typedef struct pane_width_search {
    const char *name;
    float width;
} pane_width_search;

static void find_pane_width(const lens_semantics *semantics, flux_rect bounds, lens_id id,
                            lens_id parent, void *user) {
    (void)id;
    (void)parent;
    pane_width_search *search = user;
    if (semantics->name && strcmp(semantics->name, search->name) == 0)
        search->width = bounds.w;
}

static float pane_w_via_a11y(lens *ui, const char *want) {
    pane_width_search search = {.name = want, .width = -1.0f};
    lens_accessibility_walk(ui, find_pane_width, &search);
    return search.width;
}

static void test_ratio_sizes_panes(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    for (int f = 0; f < 3; f++) {
        lens_begin(ui, &ZERO_IN);
        lens_size(ui, 400, 300);
        lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                         &(lens_split_opts){.ratio = 0.25f, .thickness = 6});
        {
            lens_split_pane(ui);
            lens_label(ui, "L");
            lens_split_pane(ui);
            lens_label(ui, "R");
        }
        lens_split_end(ui);
        lens_end(ui);
    }
    /* With display 400, thickness 6: usable = 394; first = 0.25*394 ≈ 98. */
    float wl = pane_w_via_a11y(ui, "L");
    float wr = pane_w_via_a11y(ui, "R");
    CHECK_NEAR(wl, 98.5f, 3.0f);
    CHECK(wr > wl);

    lens_destroy(ui);
}

/* Dragging the handle changes the ratio. */
static void test_drag_changes_ratio(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Build + settle for two frames so prev_rects exist. */
    for (int f = 0; f < 2; f++) {
        lens_begin(ui, &ZERO_IN);
        lens_size(ui, 400, 300);
        lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                         &(lens_split_opts){.ratio = 0.5f, .thickness = 6});
        {
            lens_split_pane(ui);
            lens_label(ui, "L");
            lens_split_pane(ui);
            lens_label(ui, "R");
        }
        lens_split_end(ui);
        lens_end(ui);
    }
    CHECK_NEAR(lens_split_ratio(ui, "s"), 0.5f, 0.001f);

    /* Press on the divider (at x ≈ 0.5*400 = 200, within the row). */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){200, 150};
    pin.mouse_pressed[LENS_MOUSE_LEFT] = true;
    pin.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &pin);
    lens_size(ui, 400, 300);
    lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                     &(lens_split_opts){.ratio = 0.5f, .thickness = 6});
    {
        lens_split_pane(ui);
        lens_label(ui, "L");
        lens_split_pane(ui);
        lens_label(ui, "R");
    }
    lens_split_end(ui);
    lens_end(ui);

    /* Drag rightward by ~80px while held. */
    lens_input din = ZERO_IN;
    din.cursor = (flux_point){280, 150};
    din.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &din);
    lens_size(ui, 400, 300);
    lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                     &(lens_split_opts){.ratio = 0.5f, .thickness = 6});
    {
        lens_split_pane(ui);
        lens_label(ui, "L");
        lens_split_pane(ui);
        lens_label(ui, "R");
    }
    lens_split_end(ui);
    lens_end(ui);

    float r = lens_split_ratio(ui, "s");
    CHECK(r > 0.55f); /* ratio grew: first pane is now wider */

    lens_destroy(ui);
}

/* A min-second floor clamps the ratio so the second pane never collapses. */
static void test_min_second_clamps(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    for (int f = 0; f < 2; f++) {
        lens_begin(ui, &ZERO_IN);
        lens_size(ui, 400, 300);
        lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                         &(lens_split_opts){.ratio = 0.5f, .thickness = 6, .min_second = 200});
        {
            lens_split_pane(ui);
            lens_label(ui, "L");
            lens_split_pane(ui);
            lens_label(ui, "R");
        }
        lens_split_end(ui);
        lens_end(ui);
    }

    /* Press on the divider and drag hard right (past the min_second floor). */
    lens_input pin = ZERO_IN;
    pin.cursor = (flux_point){200, 150};
    pin.mouse_pressed[LENS_MOUSE_LEFT] = true;
    pin.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &pin);
    lens_size(ui, 400, 300);
    lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                     &(lens_split_opts){.ratio = 0.5f, .thickness = 6, .min_second = 200});
    {
        lens_split_pane(ui);
        lens_label(ui, "L");
        lens_split_pane(ui);
        lens_label(ui, "R");
    }
    lens_split_end(ui);
    lens_end(ui);

    lens_input din = ZERO_IN;
    din.cursor = (flux_point){390, 150};
    din.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &din);
    lens_size(ui, 400, 300);
    lens_split_begin(ui, "s", LENS_SPLIT_VERTICAL,
                     &(lens_split_opts){.ratio = 0.5f, .thickness = 6, .min_second = 200});
    {
        lens_split_pane(ui);
        lens_label(ui, "L");
        lens_split_pane(ui);
        lens_label(ui, "R");
    }
    lens_split_end(ui);
    lens_end(ui);

    /* usable = 394; min_second 200 → max ratio ≈ 1 - 200/394 ≈ 0.492. The
     * drag tried to push ratio toward 1 but the floor holds it under 0.5. */
    float r = lens_split_ratio(ui, "s");
    CHECK(r <= 0.5f);

    lens_destroy(ui);
}

int main(void) {
    test_split_builds();
    test_ratio_sizes_panes();
    test_drag_changes_ratio();
    test_min_second_clamps();
    return TEST_REPORT();
}
