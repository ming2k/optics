/* test_place.c — transient placed nodes (ADR-0060): open/close persistence,
 * begin gating, anchored placement (with flip), band-order occlusion of base
 * widgets, click-outside and Escape dismissal, and centered placement. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static const lens_place_opts POPUP_AT_ZERO = {
    .band = LENS_BAND_POPUP,
    .mode = LENS_PLACE_ANCHORED,
    .rect = {0, 0, 0, 0},
    .transient = true,
};

static lens_place_opts anchored(flux_rect anchor, float pad, float min_width) {
    lens_place_opts o = POPUP_AT_ZERO;
    o.rect = anchor;
    o.layout.pad = pad;
    o.layout.min_width = min_width;
    return o;
}

static void test_open_close_persist(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    CHECK(lens_place_is_open(ui, "menu") == false);
    lens_place_open(ui, "menu");
    CHECK(lens_place_is_open(ui, "menu") == true);
    lens_end(ui);

    /* persists across frames */
    lens_begin(ui, &ZERO_IN);
    CHECK(lens_place_is_open(ui, "menu") == true);
    lens_place_close(ui, "menu");
    CHECK(lens_place_is_open(ui, "menu") == false);
    lens_end(ui);

    lens_destroy(ui);
}

/* The body only enters when the transient node is currently open. */
static void test_begin_gated_by_open_state(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int body_runs = 0;
    lens_begin(ui, &ZERO_IN);
    if (lens_place_begin(ui, "m", anchored((flux_rect){10, 10, 50, 20}, 4, 80))) {
        body_runs++;
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 0); /* closed → skipped */

    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "m");
    if (lens_place_begin(ui, "m", anchored((flux_rect){10, 10, 50, 20}, 4, 80))) {
        body_runs++;
        lens_label(ui, "item");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 1); /* open → entered */

    lens_destroy(ui);
}

static void test_open_place_reports_hover_for_its_whole_surface(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "hover-card");
    if (lens_place_begin(ui, "hover-card", anchored((flux_rect){40, 40, 40, 20}, 12, 100))) {
        lens_label(ui, "value");
        lens_place_end(ui);
    }
    lens_end(ui);

    lens_input hover = ZERO_IN;
    hover.cursor = (flux_point){50, 75};
    lens_begin(ui, &hover);
    CHECK(lens_place_hovered(ui, "hover-card"));
    if (lens_place_begin(ui, "hover-card", anchored((flux_rect){40, 40, 40, 20}, 12, 100))) {
        lens_label(ui, "value");
        lens_place_end(ui);
    }
    lens_end(ui);

    lens_place_close(ui, "hover-card");
    CHECK(!lens_place_hovered(ui, "hover-card"));
    lens_destroy(ui);
}

/* Anchored placement: by default, below the anchor; flips above when
 * there's no room. */
static void test_anchor_placement_and_flip(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    flux_rect bounds_below = {0, 0, 0, 0};
    flux_rect bounds_flip = {0, 0, 0, 0};

    /* Anchor near the top: the popup drops below it. Two frames so the
     * child's prev_rect settles, then read placement through it. */
    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "ov");
    if (lens_place_begin(ui, "ov", anchored((flux_rect){50, 20, 100, 24}, 6, 100))) {
        lens_label(ui, "row1");
        lens_label(ui, "row2");
        lens_place_end(ui);
    }
    lens_end(ui);
    lens_begin(ui, &ZERO_IN);
    if (lens_place_begin(ui, "ov", anchored((flux_rect){50, 20, 100, 24}, 6, 100))) {
        lens_label(ui, "row1");
        bounds_below = lens_get_response(ui).rect; /* row1's prev_rect */
        lens_label(ui, "row2");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(bounds_below.y >= 44.0f); /* below the anchor (20+24=44) */

    /* Now anchor at the bottom; the popup should flip ABOVE. */
    flux_rect anchor_bot = {50, 280, 100, 18}; /* y+h = 298 (near 300 bottom) */
    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "ov2");
    if (lens_place_begin(ui, "ov2", anchored(anchor_bot, 6, 100))) {
        lens_label(ui, "row1-bot");
        lens_label(ui, "row2-bot");
        lens_place_end(ui);
    }
    lens_end(ui);
    lens_begin(ui, &ZERO_IN);
    if (lens_place_begin(ui, "ov2", anchored(anchor_bot, 6, 100))) {
        lens_label(ui, "row1-bot");
        bounds_flip = lens_get_response(ui).rect;
        lens_label(ui, "row2-bot");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(bounds_flip.y < anchor_bot.y); /* flipped above */

    lens_destroy(ui);
}

/* Occlusion IS the hit-test order: a base widget under a POPUP-band node
 * is not hovered/clicked. */
static void test_higher_band_blocks_base(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts cover = anchored((flux_rect){0, 0, 200, 200}, 8, 200);
    cover.layout.bg = 0xff000000u;

    /* Frame 1: build base button + open popup covering it. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, "Base"); /* hidden under the popup */
    lens_place_open(ui, "cov");
    if (lens_place_begin(ui, "cov", cover)) {
        (void)lens_button(ui, "Top");
        lens_place_end(ui);
    }
    lens_end(ui);

    /* Frame 2: layout is settled. Press inside the popup area. The
     * base button must NOT report a click. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 30};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, "Base");
    if (lens_place_begin(ui, "cov", cover)) {
        (void)lens_button(ui, "Top");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(base_clicked == false); /* occluded by the POPUP band */

    lens_destroy(ui);
}

/* Click outside an open transient closes it (with a same-frame grace). */
static void test_click_outside_dismisses(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "m");
    if (lens_place_begin(ui, "m", anchored((flux_rect){100, 100, 80, 24}, 4, 100))) {
        lens_label(ui, "a");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(lens_place_is_open(ui, "m") == true);

    /* Frame 2: previous frame's open_frame < current frame, so the grace
     * no longer applies; the outside press closes. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){5, 5};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    if (lens_place_begin(ui, "m", anchored((flux_rect){100, 100, 80, 24}, 4, 0))) {
        lens_label(ui, "a");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(lens_place_is_open(ui, "m") == false); /* dismissed */

    lens_destroy(ui);
}

/* Escape closes the top open transient. */
static void test_escape_dismisses_top(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "a");
    lens_place_open(ui, "b");
    lens_end(ui);
    CHECK(lens_place_is_open(ui, "a"));
    CHECK(lens_place_is_open(ui, "b"));

    lens_input in = ZERO_IN;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_ESCAPE, .pressed = true};
    lens_begin(ui, &in);
    lens_end(ui);
    CHECK(lens_place_is_open(ui, "b") == false); /* top closed */
    CHECK(lens_place_is_open(ui, "a") == true);  /* below stays */

    lens_destroy(ui);
}

/* A CENTERED placed node resolves to the middle of the display (the modal
 * content path). */
static void test_centered_on_display(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_modal_open(ui, "m");
    if (lens_modal_begin(ui, "m", (lens_modal_opts){.min_width = 200})) {
        lens_label(ui, "body");
        lens_modal_end(ui);
    }
    lens_end(ui);

    lens_node *content = lens_find(ui, lens_current_id(ui, "m"));
    CHECK(content != NULL);
    flux_rect r = lens_node_bounds(content);
    CHECK_NEAR(r.x + r.w * 0.5f, 200.0f, 0.01f); /* centred in 400 */
    CHECK_NEAR(r.y + r.h * 0.5f, 150.0f, 0.01f); /* centred in 300 */
    CHECK_NEAR(r.w, 200.0f, 0.01f);              /* min_width fixed the width */

    lens_destroy(ui);
}

/* A placed node keeps its parent chain in the one tree: it appears in the
 * a11y walk under its real semantic ancestor, not bolted onto the root. */
static void test_placed_node_parents_into_tree(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_place_open(ui, "pp");
    if (lens_place_begin(ui, "pp", anchored((flux_rect){10, 10, 40, 20}, 4, 60))) {
        lens_label(ui, "inside");
        lens_place_end(ui);
    }
    lens_end(ui);

    /* The placed node is a child of the implicit root container in the
     * tree (its declaration site), and its children hang under it. */
    lens_node *root = lens_root(ui);
    CHECK(root != NULL);
    lens_node *popup = lens_find(ui, lens_current_id(ui, "pp"));
    CHECK(popup != NULL);
    CHECK(lens_node_parent(popup) == root);
    lens_node *label = lens_node_first_child(popup);
    CHECK(label != NULL);
    CHECK(lens_node_parent(label) == popup);

    lens_destroy(ui);
}

int main(void) {
    test_open_close_persist();
    test_begin_gated_by_open_state();
    test_open_place_reports_hover_for_its_whole_surface();
    test_anchor_placement_and_flip();
    test_higher_band_blocks_base();
    test_click_outside_dismisses();
    test_escape_dismisses_top();
    test_centered_on_display();
    test_placed_node_parents_into_tree();
    return TEST_REPORT();
}
