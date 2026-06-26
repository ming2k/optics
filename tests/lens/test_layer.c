/* test_layer.c — persistent floating layer (lens_layer_begin/end): the
 * chrome-panel sibling of the overlay layer. Covers the three guarantees
 * a dock / status bar / notification stack relies on:
 *   - always rendered (no open state, body always entered),
 *   - placed at the exact rect (no below-anchor drop, no flip),
 *   - not dismissible (Escape and click-outside leave it alone).
 * Plus eclipse: a base widget under a layer is shielded, mirroring the
 * overlay behaviour. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* The body always runs — no open/close state to seed. */
static void test_always_rendered(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int body_runs = 0;
    lens_begin(ui, &ZERO_IN);
    if (lens_layer_begin(ui, "dock", (flux_rect){100, 260, 200, 36},
                         (lens_overlay_opts){.pad = 6, .min_width = 200})) {
        body_runs++;
        lens_label(ui, "tile");
        lens_layer_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 1);

    /* Still entered next frame, with no open call. */
    lens_begin(ui, &ZERO_IN);
    if (lens_layer_begin(ui, "dock", (flux_rect){100, 260, 200, 36},
                         (lens_overlay_opts){.pad = 6})) {
        body_runs++;
        lens_label(ui, "tile");
        lens_layer_end(ui);
    }
    lens_end(ui);
    CHECK(body_runs == 2);

    lens_destroy(ui);
}

/* Placement is exactly at the rect's top-left — no below-anchor drop. */
static void test_place_at_rect(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: build the layer so prev_rect is seeded. */
    lens_begin(ui, &ZERO_IN);
    if (lens_layer_begin(ui, "bar", (flux_rect){40, 50, 120, 24},
                         (lens_overlay_opts){.pad = 4, .min_width = 120})) {
        lens_label(ui, "x");
        lens_layer_end(ui);
    }
    lens_end(ui);

    /* Frame 2: read the layer's settled geometry via its child's
     * prev_rect (one-frame latency, same trick test_overlay uses). */
    flux_rect label_rect = {0, 0, 0, 0};
    lens_begin(ui, &ZERO_IN);
    if (lens_layer_begin(ui, "bar", (flux_rect){40, 50, 120, 24},
                         (lens_overlay_opts){.pad = 4, .min_width = 120})) {
        lens_label(ui, "x");
        label_rect = lens_get_response(ui).rect;
        lens_layer_end(ui);
    }
    lens_end(ui);

    /* The label sits inside the panel, padded by 4 — so it must be at
     * or below y == 50 (the rect's top). The overlay path would have
     * dropped it to y >= 74 (50 + 24); the layer path keeps it at 50+pad. */
    CHECK(label_rect.y >= 50.0f);
    CHECK(label_rect.y < 74.0f);
    /* And horizontally anchored at the rect's left edge (>= 40). */
    CHECK(label_rect.x >= 40.0f);

    lens_destroy(ui);
}

/* Escape and click-outside must not close a persistent layer — it has
 * no open state to lose. */
static void test_not_dismissible(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Build a layer; assert it renders. */
    lens_begin(ui, &ZERO_IN);
    if (lens_layer_begin(ui, "wsbar", (flux_rect){0, 0, 400, 28}, (lens_overlay_opts){.pad = 0})) {
        lens_label(ui, "ws1");
        lens_layer_end(ui);
    }
    lens_end(ui);

    /* Press Escape the next frame. */
    lens_input in_esc = ZERO_IN;
    in_esc.key_count = 1;
    in_esc.keys[0] = (lens_key_event){.key = LENS_KEY_ESCAPE, .pressed = true};
    lens_begin(ui, &in_esc);
    int runs_after_esc = 0;
    if (lens_layer_begin(ui, "wsbar", (flux_rect){0, 0, 400, 28}, (lens_overlay_opts){.pad = 0})) {
        runs_after_esc++;
        lens_label(ui, "ws1");
        lens_layer_end(ui);
    }
    lens_end(ui);
    CHECK(runs_after_esc == 1); /* Escape did not dismiss the layer */

    /* Click outside the layer's rect. */
    lens_input in_click = ZERO_IN;
    in_click.cursor = (flux_point){200, 200}; /* outside the 0..400,0..28 bar */
    in_click.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in_click.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in_click);
    int runs_after_click = 0;
    if (lens_layer_begin(ui, "wsbar", (flux_rect){0, 0, 400, 28}, (lens_overlay_opts){.pad = 0})) {
        runs_after_click++;
        lens_label(ui, "ws1");
        lens_layer_end(ui);
    }
    lens_end(ui);
    CHECK(runs_after_click == 1); /* click-outside did not dismiss either */

    lens_destroy(ui);
}

/* A base widget under a persistent layer is eclipsed, just as it would
 * be under an open overlay — so a click on the dock does not fall
 * through to the window behind it. */
static void test_eclipse_blocks_base(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: base button + layer covering it. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, "Base");
    if (lens_layer_begin(ui, "cov", (flux_rect){0, 0, 200, 200},
                         (lens_overlay_opts){.pad = 8, .min_width = 200, .bg = 0xff000000})) {
        (void)lens_button(ui, "Top");
        lens_layer_end(ui);
    }
    lens_end(ui);

    /* Frame 2: settled geometry. Press inside the layer area; the base
     * button must NOT report a click. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 30};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, "Base");
    if (lens_layer_begin(ui, "cov", (flux_rect){0, 0, 200, 200},
                         (lens_overlay_opts){.pad = 8, .min_width = 200, .bg = 0xff000000})) {
        (void)lens_button(ui, "Top");
        lens_layer_end(ui);
    }
    lens_end(ui);
    CHECK(base_clicked == false);

    lens_destroy(ui);
}

int main(void) {
    test_always_rendered();
    test_place_at_rect();
    test_not_dismissible();
    test_eclipse_blocks_base();
    return TEST_REPORT();
}
