/* test_overlay.c — floating layer: open/close persistence, begin gating,
 * anchored placement (with flip), eclipse of base widgets, click-outside
 * and Escape dismissal (ADR-0014). */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static void test_open_close_persist(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    CHECK(lens_overlay_is_open(ui, "menu") == false);
    lens_overlay_open(ui, "menu");
    CHECK(lens_overlay_is_open(ui, "menu") == true);
    lens_end(ui);

    /* persists across frames */
    lens_begin(ui, &ZERO_IN);
    CHECK(lens_overlay_is_open(ui, "menu") == true);
    lens_overlay_close(ui, "menu");
    CHECK(lens_overlay_is_open(ui, "menu") == false);
    lens_end(ui);

    lens_destroy(ui);
}

/* The body only enters when the overlay is currently open. */
static void test_begin_gated_by_open_state(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int body_runs = 0;
    lens_begin(ui, &ZERO_IN);
    if (lens_overlay_begin(ui, "m", (flux_rect){10, 10, 50, 20},
                           (lens_overlay_opts){.pad = 4, .min_width = 80})) {
        body_runs++;
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 0); /* closed → skipped */

    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "m");
    if (lens_overlay_begin(ui, "m", (flux_rect){10, 10, 50, 20},
                           (lens_overlay_opts){.pad = 4, .min_width = 80})) {
        body_runs++;
        lens_label(ui, "item");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 1); /* open → entered */

    lens_destroy(ui);
}

static void test_open_overlay_reports_hover_for_its_whole_surface(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "hover-card");
    if (lens_overlay_begin(ui, "hover-card", (flux_rect){40, 40, 40, 20},
                           (lens_overlay_opts){.pad = 12, .min_width = 100})) {
        lens_label(ui, "value");
        lens_overlay_end(ui);
    }
    lens_end(ui);

    lens_input hover = ZERO_IN;
    hover.cursor = (flux_point){50, 75};
    lens_begin(ui, &hover);
    CHECK(lens_overlay_hovered(ui, "hover-card"));
    if (lens_overlay_begin(ui, "hover-card", (flux_rect){40, 40, 40, 20},
                           (lens_overlay_opts){.pad = 12, .min_width = 100})) {
        lens_label(ui, "value");
        lens_overlay_end(ui);
    }
    lens_end(ui);

    lens_overlay_close(ui, "hover-card");
    CHECK(!lens_overlay_hovered(ui, "hover-card"));
    lens_destroy(ui);
}

/* Anchored placement: by default, below the anchor; flips above when
 * there's no room. */
static void test_anchor_placement_and_flip(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    flux_rect bounds_below = {0, 0, 0, 0};
    flux_rect bounds_flip = {0, 0, 0, 0};

    /* Anchor near the top: layer drops below it. */
    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "ov");
    if (lens_overlay_begin(ui, "ov", (flux_rect){50, 20, 100, 24},
                           (lens_overlay_opts){.pad = 6, .min_width = 100})) {
        lens_label(ui, "row1");
        lens_label(ui, "row2");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    /* Walk overlay layers via the a11y export — semantics' parent is the
     * overlay's nearest semantic ancestor, but bounds are reported per
     * node. We expect the layer's two labels to live below y == 44. */
    /* (Walk inspected indirectly: in a flip test the labels appear above
     *  the anchor instead, so we test bounds by inspecting lens_find on
     *  a known label id — but labels are anonymous. Use the layer node
     *  via the *response of the last widget* as a stand-in for placement
     *  is brittle. Inspect bounds via the next frame's prev_rect: a
     *  child label has has_prev set after one frame.) */
    /* second frame to settle one-frame layout */
    lens_begin(ui, &ZERO_IN);
    if (lens_overlay_begin(ui, "ov", (flux_rect){50, 20, 100, 24},
                           (lens_overlay_opts){.pad = 6, .min_width = 100})) {
        lens_label(ui, "row1");
        bounds_below = lens_get_response(ui).rect; /* row1's prev_rect */
        lens_label(ui, "row2");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(bounds_below.y >= 44.0f); /* below the anchor (20+24=44) */

    /* Now anchor at the bottom; layer should flip ABOVE. Two frames again. */
    flux_rect anchor_bot = {50, 280, 100, 18}; /* y+h = 298 (near 300 bottom) */
    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "ov2");
    if (lens_overlay_begin(ui, "ov2", anchor_bot,
                           (lens_overlay_opts){.pad = 6, .min_width = 100})) {
        lens_label(ui, "row1-bot");
        lens_label(ui, "row2-bot");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    lens_begin(ui, &ZERO_IN);
    if (lens_overlay_begin(ui, "ov2", anchor_bot,
                           (lens_overlay_opts){.pad = 6, .min_width = 100})) {
        lens_label(ui, "row1-bot");
        bounds_flip = lens_get_response(ui).rect;
        lens_label(ui, "row2-bot");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(bounds_flip.y < anchor_bot.y); /* flipped above */

    lens_destroy(ui);
}

/* Eclipse: a base widget under an open overlay is not hovered/clicked. */
static void test_eclipse_blocks_base(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: build base button + open overlay covering it. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, "Base"); /* hidden under overlay */
    lens_overlay_open(ui, "cov");
    if (lens_overlay_begin(ui, "cov", (flux_rect){0, 0, 200, 200},
                           (lens_overlay_opts){.pad = 8, .min_width = 200, .bg = 0xff000000})) {
        (void)lens_button(ui, "Top");
        lens_overlay_end(ui);
    }
    lens_end(ui);

    /* Frame 2: layout is settled. Press inside the overlay area. The
     * base button must NOT report a click; the *top* button should. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 30};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, "Base");
    lens_overlay_open(ui, "cov");
    if (lens_overlay_begin(ui, "cov", (flux_rect){0, 0, 200, 200},
                           (lens_overlay_opts){.pad = 8, .min_width = 200, .bg = 0xff000000})) {
        (void)lens_button(ui, "Top");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(base_clicked == false); /* eclipsed by overlay */

    lens_destroy(ui);
}

/* Click outside an open overlay closes it (with a same-frame grace). */
static void test_click_outside_dismisses(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "m");
    if (lens_overlay_begin(ui, "m", (flux_rect){100, 100, 80, 24},
                           (lens_overlay_opts){.pad = 4, .min_width = 100})) {
        lens_label(ui, "a");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "m") == true);

    /* Same-frame-as-open: a press elsewhere should NOT dismiss (grace). */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){5, 5};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    /* Frame 2: previous frame's open_frame == frame-1 < current frame, so
     * the grace condition is now satisfied; the outside press will close. */
    lens_begin(ui, &in);
    if (lens_overlay_begin(ui, "m", (flux_rect){100, 100, 80, 24}, (lens_overlay_opts){.pad = 4})) {
        lens_label(ui, "a");
        lens_overlay_end(ui);
    }
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "m") == false); /* dismissed */

    lens_destroy(ui);
}

/* Escape closes the top open overlay. */
static void test_escape_dismisses_top(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_overlay_open(ui, "a");
    lens_overlay_open(ui, "b");
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "a"));
    CHECK(lens_overlay_is_open(ui, "b"));

    lens_input in = ZERO_IN;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_ESCAPE, .pressed = true};
    lens_begin(ui, &in);
    lens_end(ui);
    CHECK(lens_overlay_is_open(ui, "b") == false); /* top closed */
    CHECK(lens_overlay_is_open(ui, "a") == true);  /* below stays */

    lens_destroy(ui);
}

int main(void) {
    test_open_close_persist();
    test_begin_gated_by_open_state();
    test_open_overlay_reports_hover_for_its_whole_surface();
    test_anchor_placement_and_flip();
    test_eclipse_blocks_base();
    test_click_outside_dismisses();
    test_escape_dismisses_top();
    return TEST_REPORT();
}
