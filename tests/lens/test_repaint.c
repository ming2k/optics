/* test_repaint.c — lens_frame_needs_repaint: damage-driven repaint query.
 * A changed leaf (same id, new content) must report true; a pure time
 * frame with identical content must report false. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void build_frame(lens *ui, float value) {
    lens_begin(ui, &IN0);
    lens_label(ui, "status");
    lens_progress(ui, "load", value);
    lens_end(ui);
}

static void render_frame(lens *ui, flux_canvas *canvas) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(canvas, &clear) == FLUX_OK);
    CHECK(lens_render(ui, canvas) == FLUX_OK);
    flux_canvas_cpu_end(canvas);
}

static void test_null_safety(void) {
    CHECK(!lens_frame_needs_repaint(NULL));
}

static void test_first_frames_repaint_then_settle(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    build_frame(ui, 0.5f);
    CHECK(lens_frame_needs_repaint(ui)); /* entering nodes: first paint */
    render_frame(ui, canvas);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    /* Stable nodes, identical geometry and draw lists, only dt advanced:
     * nothing to repaint. */
    CHECK(!lens_frame_needs_repaint(ui));

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

static void test_leaf_change_repaints(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    CHECK(!lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Same widget id, new value: the draw list changes. */
    build_frame(ui, 0.75f);
    CHECK(lens_frame_needs_repaint(ui));

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

static void test_place_open_close_repaints(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    const lens_place_opts popup = {
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_ANCHORED,
        .rect = {10, 10, 50, 20},
        .transient = true,
        .layout = {.pad = 4, .min_width = 80},
    };

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    CHECK(!lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Open a transient popup: the tree's child set changes even though
     * the base widgets are untouched. */
    lens_begin(ui, &IN0);
    lens_label(ui, "status");
    lens_progress(ui, "load", 0.5f);
    lens_place_open(ui, "menu");
    if (lens_place_begin(ui, "menu", popup)) {
        lens_label(ui, "item");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Let the popup settle: identical frames must stop reporting. */
    for (int i = 0; i < 3; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, "status");
        lens_progress(ui, "load", 0.5f);
        if (lens_place_begin(ui, "menu", popup)) {
            lens_label(ui, "item");
            lens_place_end(ui);
        }
        lens_end(ui);
        render_frame(ui, canvas);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    /* Close it: the popup's pixels must be erased. */
    lens_begin(ui, &IN0);
    lens_label(ui, "status");
    lens_progress(ui, "load", 0.5f);
    lens_place_close(ui, "menu");
    if (lens_place_begin(ui, "menu", popup)) {
        lens_label(ui, "item");
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(lens_frame_needs_repaint(ui));

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

int main(void) {
    test_null_safety();
    test_first_frames_repaint_then_settle();
    test_leaf_change_repaints();
    test_place_open_close_repaints();
    return TEST_REPORT();
}
