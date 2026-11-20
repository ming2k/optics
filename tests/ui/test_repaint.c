/* test_repaint.c — lens_frame_needs_repaint: damage-driven repaint query. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <stdio.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Roomier surface for the removed-subtree tests: a departed band must sit in a
 * region no surviving widget happens to overlap. */
static const optics_ui_input WIDE_IN = {.display_size = {900, 400}, .dt_seconds = 0.016f};

/* Two placed bands at opposite edges: the shape of a status strip plus a dock.
 * Their union is the whole viewport, which is why they must stay separate. */
static const optics_ui_place_opts TOP_BAND = {
    .box = {.id = "top-band"},
    .band = OPTICS_UI_BAND_CHROME,
    .mode = OPTICS_UI_PLACE_EXACT,
    .rect = {0, 0, 400, 24},
    .layout = {.box = {.min_width = 400, .min_height = 24}, .pad = 2},
};

static const optics_ui_place_opts BOTTOM_BAND = {
    .box = {.id = "bottom-band"},
    .band = OPTICS_UI_BAND_CHROME,
    .mode = OPTICS_UI_PLACE_EXACT,
    .rect = {0, 160, 400, 40},
    .layout = {.box = {.min_width = 400, .min_height = 40}, .pad = 2},
};

static void build_frame(optics_ui *ui, float value) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)value);
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = val_str, .box = {.id = "val_node"}});
    optics_ui_test_end(ui);
}

static void render_frame(optics_ui *ui, optics_gfx_canvas *canvas) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
}

static bool rect_covers(optics_gfx_rect outer, optics_gfx_rect inner) {
    return outer.x <= inner.x && outer.y <= inner.y && outer.x + outer.w >= inner.x + inner.w &&
           outer.y + outer.h >= inner.y + inner.h;
}

/* The frame's damage list, via the public count-then-fill contract. Returns a
 * buffer with static lifetime so callers can inspect it after the helper. */
static const optics_gfx_rect *damage_rects(const optics_ui *ui, uint32_t *count) {
    static optics_gfx_rect buffer[OPTICS_UI_DAMAGE_RECTS_CAPACITY];
    *count = optics_ui_damage_rects(ui, &(optics_ui_damage_opts){
                                            .rects = buffer,
                                            .capacity = OPTICS_UI_DAMAGE_RECTS_CAPACITY,
                                        });
    return buffer;
}

static void test_null_safety(void) {
    OT_EXPECT(!optics_ui_frame_needs_repaint(nullptr));
}

static void test_first_frames_repaint_then_settle(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    build_frame(ui, 0.5f);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_leaf_change_repaints(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Same widget id, new value: the draw list changes. */
    build_frame(ui, 0.75f);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_place_open_close_repaints(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_ui_place_opts popup = {
        .box = {.id = "menu"},
        .band = OPTICS_UI_BAND_POPUP,
        .mode = OPTICS_UI_PLACE_ANCHORED,
        .rect = {10, 10, 50, 20},
        .transient = true,
        .layout = {.box = {.min_width = 80}, .pad = 4},
    };

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Open a transient popup: the tree's child set changes */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    optics_ui_place_open(ui, "menu");
    if (optics_ui_place_begin(ui, &popup)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Let the popup settle */
    for (int i = 0; i < 3; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
        if (optics_ui_place_begin(ui, &popup)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    /* Close it: the popup's pixels must be erased. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    optics_ui_place_close(ui, "menu");
    if (optics_ui_place_begin(ui, &popup)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_damage_rect_bounds(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    /* Before anything is built there is nothing to describe. */
    uint32_t count = 0;
    OT_EXPECT(damage_rects(ui, &count) == nullptr || count == 0);
    OT_EXPECT(optics_ui_damage_rects(ui, &(optics_ui_damage_opts){}) == 0);

    build_frame(ui, 0.5f);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    const optics_gfx_rect *first = damage_rects(ui, &count);
    OT_EXPECT(count > 0);
    OT_EXPECT(first[0].w > 0.0f && first[0].h > 0.0f);
    render_frame(ui, canvas);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));
    OT_EXPECT(optics_ui_damage_rects(ui, &(optics_ui_damage_opts){}) == 0);
    render_frame(ui, canvas);

    /* Mutate leaf: every reported region must be inside the viewport. */
    build_frame(ui, 1.25f);
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    const optics_gfx_rect *dirty = damage_rects(ui, &count);
    OT_EXPECT(count > 0);
    for (uint32_t i = 0; i < count; i++) {
        OT_EXPECT(dirty[i].w > 0.0f && dirty[i].h > 0.0f);
        OT_EXPECT(dirty[i].x >= 0.0f && dirty[i].y >= 0.0f);
        OT_EXPECT(dirty[i].x + dirty[i].w <= 400.0f);
        OT_EXPECT(dirty[i].y + dirty[i].h <= 200.0f);
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_placed_band_damage_rect_bounds(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 1920,
                                                           .height = 1080},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_ui_place_opts chrome = {
        .box = {.id = "dock"},
        .band = OPTICS_UI_BAND_CHROME,
        .mode = OPTICS_UI_PLACE_ANCHORED,
        .rect = {0, 950, 500, 60},
        .layout = {.box = {.min_width = 500, .min_height = 60}, .pad = 4},
    };

    /* Build first frame and settle */
    optics_ui_begin(ui, &IN0);
    if (optics_ui_place_begin(ui, &chrome)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    render_frame(ui, canvas);

    for (int i = 0; i < 3; i++) {
        optics_ui_begin(ui, &IN0);
        if (optics_ui_place_begin(ui, &chrome)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    /* Mutate the placed chrome node label */
    optics_ui_begin(ui, &IN0);
    if (optics_ui_place_begin(ui, &chrome)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "modified"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 1);
    OT_EXPECT(rects[0].w > 0.0f && rects[0].h > 0.0f);
    /* Damage must stay inside the placed chrome band, NOT the full screen
     * (400x200). The band resolves to y = 140 after the ANCHORED flip and the
     * viewport clamp; the label that changed paints inside it. */
    OT_EXPECT(rects[0].w <= 400.0f);
    OT_EXPECT(rects[0].h <= 60.0f);
    OT_EXPECT(rects[0].y >= 140.0f);
    OT_EXPECT(rects[0].y + rects[0].h <= 200.0f);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* A frame may change several unrelated regions at once. Collapsing them into one
 * bounding box is what makes a status tick and a dock spring re-composite the
 * whole output, so the list must stay disjoint (ADR-0123). */
static void test_disjoint_bands_stay_disjoint(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    uint32_t count = 0;

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        if (optics_ui_place_begin(ui, &TOP_BAND)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "top"});
            optics_ui_place_end(ui);
        }
        if (optics_ui_place_begin(ui, &BOTTOM_BAND)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "bottom"});
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    /* Touch both bands. */
    optics_ui_begin(ui, &IN0);
    if (optics_ui_place_begin(ui, &TOP_BAND)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "top!"});
        optics_ui_place_end(ui);
    }
    if (optics_ui_place_begin(ui, &BOTTOM_BAND)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "bottom!"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 2);
    if (count == 2) {
        /* Sorted top-left first, and the two never merge: the union would be the
         * full 200 px height the bands are trying to avoid. */
        OT_EXPECT(rects[0].y < rects[1].y);
        OT_EXPECT(rects[0].y + rects[0].h <= rects[1].y);
        /* The two need not overlap in y, so a single-rect consumer would have to
         * span the gap between them. */
        OT_EXPECT(rects[0].h + rects[1].h < 200.0f);
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* A host whose presentation API takes one rectangle must collapse the list
 * itself. This test pins the property that makes that collapse a deliberate,
 * local decision: the list is strictly more informative than its union, so
 * collapsing inside the producer would destroy information no consumer could
 * recover. */
static void test_the_list_is_strictly_more_informative_than_its_union(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        if (optics_ui_place_begin(ui, &TOP_BAND)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "top"});
            optics_ui_place_end(ui);
        }
        if (optics_ui_place_begin(ui, &BOTTOM_BAND)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "bottom"});
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }

    optics_ui_begin(ui, &IN0);
    if (optics_ui_place_begin(ui, &TOP_BAND)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "top!"});
        optics_ui_place_end(ui);
    }
    if (optics_ui_place_begin(ui, &BOTTOM_BAND)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "bottom!"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 2);
    optics_gfx_rect collapsed = {0, 0, 0, 0};
    int64_t covered = 0;
    for (uint32_t i = 0; i < count; i++) {
        collapsed = optics_gfx_rect_union(collapsed, rects[i]);
        covered += (int64_t)rects[i].w * (int64_t)rects[i].h;
    }
    const int64_t boxed = (int64_t)collapsed.w * (int64_t)collapsed.h;
    OT_EXPECT(boxed > covered * 4);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* An edited widget reports its own paint, not the layout box it was arranged
 * into. A label laid out across a stretch column must not claim the column: the
 * column's rect is the whole display, and that is exactly the over-report that
 * turned one terminal cell into a full-output composite. */
static void test_flow_leaf_reports_its_glyphs_not_its_column(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 1920,
                                                           .height = 1080},
                                 &canvas) == OPTICS_GFX_OK);

    static const optics_ui_input BIG = {.display_size = {1920, 1080}, .dt_seconds = 0.016f};
    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &BIG);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "cell"});
        optics_ui_test_end(ui);
        optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    optics_ui_begin(ui, &BIG);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "celu"});
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 1);
    if (count == 1) {
        OT_EXPECT(rects[0].w > 0.0f);
        OT_EXPECT(rects[0].w < 200.0f);
        OT_EXPECT(rects[0].h < 60.0f);
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* Centred text paints at the middle of its box, not at its corner. A damage rect
 * taken from the box corner (or from the raw `rel` rectangle) reports pixels the
 * glyphs never touch and omits the ones they do — a stale-pixel bug that a
 * left-aligned-only test suite cannot see.
 *
 * This is the regression test for exactly that: the first implementation ignored
 * the `rel.w < 0` centring sentinel and reported the run at x = 0. */
static void test_centered_text_damage_follows_the_glyphs(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_ui_label_opts centred = {
        .text = "AB",
        .align = OPTICS_UI_CENTER,
        .box = {.id = "centred", .min_width = 380.0f},
    };

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &centred);
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "AC",
                                                .align = OPTICS_UI_CENTER,
                                                .box = {.id = "centred", .min_width = 380.0f}});
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 1);
    if (count == 1) {
        /* The run is centred in the 380px box, so its left edge is far from 0. */
        OT_EXPECT(rects[0].x > 100.0f);
        OT_EXPECT(rects[0].x + rects[0].w < 300.0f);
        OT_EXPECT(rects[0].w > 0.0f);
        OT_EXPECT(rects[0].w < 100.0f);
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* A caller sizes its buffer from the count-only query, then fills it. Both
 * answers come from the same walk and must agree. */
static void test_count_only_query_matches_the_written_list(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.75f);

    const uint32_t asked =
        optics_ui_damage_rects(ui, &(optics_ui_damage_opts){.rects = nullptr, .capacity = 0});
    OT_EXPECT(asked > 0);
    optics_gfx_rect buffer[OPTICS_UI_DAMAGE_RECTS_CAPACITY];
    const uint32_t written =
        optics_ui_damage_rects(ui, &(optics_ui_damage_opts){
                                       .rects = buffer,
                                       .capacity = OPTICS_UI_DAMAGE_RECTS_CAPACITY,
                                   });
    OT_EXPECT(written == asked);

    /* A buffer too small reports the whole set as one box rather than a
     * truncated list: a dropped region leaves stale pixels on screen. */
    optics_gfx_rect one[1];
    const uint32_t trimmed = optics_ui_damage_rects(ui, &(optics_ui_damage_opts){
                                                            .rects = one,
                                                            .capacity = 1,
                                                        });
    OT_EXPECT(trimmed == 1);
    OT_EXPECT(one[0].w > 0.0f && one[0].h > 0.0f);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* More distinct regions than the public contract carries must degrade to one
 * bounding box, never to a truncated list and never by reading past the fixed
 * accumulation buffer.
 *
 * This is the regression test for a bounds bug in the first implementation:
 * the accumulator counted rectangles it could not store and the normalizer was
 * handed that larger count, so it read uninitialized stack memory. */
static void test_overflow_collapses_to_one_bounding_box(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    /* Wide enough for more than LENS_DAMAGE_RECTS_CAPACITY disjoint leaves. */
    const uint32_t columns = OPTICS_UI_DAMAGE_RECTS_CAPACITY + 16;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = (float)columns * 8.0f,
                                                           .height = 200.0f},
                                 &canvas) == OPTICS_GFX_OK);

    static optics_ui_input wide = {.display_size = {0, 200}, .dt_seconds = 0.016f};
    wide.display_size.x = (float)columns * 8.0f;

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &wide);
        optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 0.0f});
        for (uint32_t c = 0; c < columns; c++) {
            /* Distinct ids: retained ids name nodes one-to-one, so a repeated
             * literal would collapse the whole row into a single node. */
            optics_ui_push_id_int(ui, (int64_t)c);
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "x", .box = {.id = "cell"}});
            optics_ui_pop_id(ui);
        }
        optics_ui_row_end(ui);
        optics_ui_test_end(ui);
        optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    }
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    /* Change every cell so the region count exceeds the contract. */
    optics_ui_begin(ui, &wide);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 0.0f});
    for (uint32_t c = 0; c < columns; c++) {
        optics_ui_push_id_int(ui, (int64_t)c);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "y", .box = {.id = "cell"}});
        optics_ui_pop_id(ui);
    }
    optics_ui_row_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect buffer[OPTICS_UI_DAMAGE_RECTS_CAPACITY];
    for (uint32_t i = 0; i < OPTICS_UI_DAMAGE_RECTS_CAPACITY; i++)
        buffer[i] = (optics_gfx_rect){-1.0f, -1.0f, -1.0f, -1.0f};

    const uint32_t count =
        optics_ui_damage_rects(ui, &(optics_ui_damage_opts){
                                       .rects = buffer,
                                       .capacity = OPTICS_UI_DAMAGE_RECTS_CAPACITY,
                                   });
    /* Never more rectangles than the caller's capacity. */
    OT_EXPECT(count <= OPTICS_UI_DAMAGE_RECTS_CAPACITY);
    OT_EXPECT(count > 0);
    /* Every slot the function claims to have written must hold a real rect. */
    for (uint32_t i = 0; i < count; i++) {
        OT_EXPECT(buffer[i].w > 0.0f);
        OT_EXPECT(buffer[i].h > 0.0f);
        OT_EXPECT(buffer[i].x + buffer[i].w <= wide.display_size.x);
    }
    /* Overflow is reported as one full-viewport rect: the omitted regions are
     * unknown, so a list or box built only from the survivors could under-cover
     * them. 65+ disjoint cells cannot fit the 64-rect contract, so this frame
     * must collapse. */
    OT_EXPECT(count == 1);
    OT_EXPECT_NEAR(buffer[0].x, 0.0f, 0.01f);
    OT_EXPECT_NEAR(buffer[0].y, 0.0f, 0.01f);
    OT_EXPECT_NEAR(buffer[0].w, wide.display_size.x, 0.01f);
    OT_EXPECT_NEAR(buffer[0].h, wide.display_size.y, 0.01f);

    /* A one-rect buffer must likewise answer with a single covering box. */
    optics_gfx_rect one[1] = {{-1.0f, -1.0f, -1.0f, -1.0f}};
    const uint32_t trimmed = optics_ui_damage_rects(ui, &(optics_ui_damage_opts){
                                                            .rects = one,
                                                            .capacity = 1,
                                                        });
    OT_EXPECT(trimmed == 1);
    OT_EXPECT(one[0].w > 0.0f && one[0].h > 0.0f);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_damage_rects_null_safety(void) {
    OT_EXPECT(optics_ui_damage_rects(nullptr, nullptr) == 0);
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    /* No frame built yet: no damage, and a null opts must not dereference. */
    OT_EXPECT(optics_ui_damage_rects(ui, nullptr) == 0);
    optics_ui_release(ui);
}

/* A node removed from a FLOW container leaves pixels no other widget covers.
 *
 * The node is detached from its parent's child list immediately but stays in
 * the store for the leave grace window, so a damage walk over the tree cannot
 * reach it. Only a store sweep finds it. Regression for the region that stayed
 * frozen until some unrelated full repaint happened to cover it. */
static void test_removed_child_repaints_its_vacated_band(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    char tick[32];
    optics_gfx_rect row_c = {};

    /* Settle a three-row list and capture the row's real bounds. The exact
     * glyph advance depends on the host's font build, so a hard-coded rect
     * would fail on a different FreeType/HarfBuzz version for reasons that
     * have nothing to do with damage. */
    for (int i = 0; i < 4; i++) {
        snprintf(tick, sizeof tick, "tick=%d", i);
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = tick, .box = {.id = "ticker"}});
        optics_ui_column_begin(ui,
                               &(optics_ui_layout_opts){.box = {.id = "list", .min_width = 120}});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-a"});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-b"});
        optics_ui_response row_c_node =
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-c"});
        optics_ui_column_end(ui);
        optics_ui_test_end(ui);
        if (i == 3) {
            OT_EXPECT(row_c_node.id != 0);
            row_c = optics_ui_node_bounds(optics_ui_find(ui, row_c_node.id));
        }
        render_frame(ui, canvas);
    }
    OT_EXPECT(row_c.w > 0.0f && row_c.h > 0.0f);

    /* Drop the trailing row. The ticker changes in the same frame, so the
     * "no region named" viewport fallback cannot mask an omission here. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "tick=9", .box = {.id = "ticker"}});
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.id = "list", .min_width = 120}});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-a"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-b"});
    optics_ui_column_end(ui);
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count > 0);

    /* The vacated band must be covered by at least one reported rect. */
    bool covered = false;
    for (uint32_t i = 0; i < count && !covered; i++)
        covered = rect_covers(rects[i], row_c);
    OT_EXPECT(covered);

    /* Localization holds: a couple of small strips, not the whole display the
     * pre-ADR-0143 fallback would have reported. */
    OT_EXPECT(count <= 4);
    for (uint32_t i = 0; i < count; i++)
        OT_EXPECT(rects[i].w < 200.0f);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* A removed subtree is erased through its children: the departed container
 * paints nothing itself, so its own bounds are not where the pixels are. */
static void test_removed_container_repaints_its_children(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "header", .box = {.id = "header"}});
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.id = "panel"}});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "inner-text"});
        optics_ui_column_end(ui);
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }

    /* Remove the whole panel; the header changes with it. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "header2", .box = {.id = "header"}});
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count > 0);
    /* `inner-text` is laid out below the header, so some rect must reach past
     * the header's band. */
    bool below = false;
    for (uint32_t i = 0; i < count && !below; i++)
        below = rects[i].y + rects[i].h > 20.0f;
    OT_EXPECT(below);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* The departed region must not depend on what else the frame built.
 *
 * A LEAVING node's command list lives in the per-frame arena, which `lens_begin`
 * rewinds and immediately recycles. Reading damage out of it made the reported
 * region follow whatever this frame happened to allocate there: measured, one
 * unrelated label appearing elsewhere moved a departed row's damage from
 * `{0,65 36x20}` to `{0,73 291x20}`. Damage must come from snapshot-persisted
 * geometry, and this asserts the invariance directly — same UI history, only
 * the amount of unrelated content differs.
 */
static void test_departed_damage_is_independent_of_unrelated_content(void) {
    optics_gfx_rect expect = {};
    bool have_expect = false;
    for (int fillers = 0; fillers <= 40; fillers = fillers ? fillers * 8 : 1) {
        optics_ui *ui = nullptr;
        optics_gfx_canvas *canvas = nullptr;
        OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = 900,
                                                .height = 400},
                      &canvas) == OPTICS_GFX_OK);

        optics_gfx_rect row_c = {};
        for (int i = 0; i < 4; i++) {
            optics_ui_begin(ui, &WIDE_IN);
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "ticker", .box = {.id = "ticker"}});
            optics_ui_column_begin(
                ui, &(optics_ui_layout_opts){.box = {.id = "list", .min_width = 120}});
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-a"});
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-b"});
            optics_ui_response row_c_node =
                optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-c"});
            optics_ui_column_end(ui);
            optics_ui_test_end(ui);
            if (i == 3) {
                OT_EXPECT(row_c_node.id != 0);
                row_c = optics_ui_node_bounds(optics_ui_find(ui, row_c_node.id));
            }
            render_frame(ui, canvas);
        }
        OT_EXPECT(row_c.w > 0.0f && row_c.h > 0.0f);

        optics_ui_begin(ui, &WIDE_IN);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "tick=9", .box = {.id = "ticker"}});
        optics_ui_column_begin(ui,
                               &(optics_ui_layout_opts){.box = {.id = "list", .min_width = 120}});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-a"});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row-b"});
        optics_ui_column_end(ui);
        for (int i = 0; i < fillers; i++)
            optics_ui_label(
                ui, &(optics_ui_label_opts){.text = "the quick brown fox jumps over 0123456789",
                                            .box = {.id = "filler"}});
        optics_ui_test_end(ui);

        uint32_t count = 0;
        const optics_gfx_rect *rects = damage_rects(ui, &count);
        optics_gfx_rect departed = {};
        for (uint32_t i = 0; i < count && departed.w == 0.0f; i++)
            if (rect_covers(rects[i], row_c))
                departed = rects[i];
        OT_EXPECT(departed.w > 0.0f && departed.h > 0.0f);

        /* The first iteration defines the expected region; unrelated content
         * must not move or resize it on later iterations. */
        if (!have_expect) {
            expect = departed;
            have_expect = true;
        } else {
            OT_EXPECT_NEAR(departed.x, expect.x, 0.01f);
            OT_EXPECT_NEAR(departed.y, expect.y, 0.01f);
            OT_EXPECT_NEAR(departed.w, expect.w, 0.01f);
            OT_EXPECT_NEAR(departed.h, expect.h, 0.01f);
        }

        optics_gfx_canvas_release(canvas);
        optics_ui_release(ui);
    }
}

/* A tooltip is painted *below* its anchor, with padding — so reporting the anchor
 * reports a band the tooltip does not occupy and misses the one it does. This is
 * not hypothetical: the anchor is the widget's own rect, the painted box starts at
 * `anchor.y + anchor.h + 4`, and neither contains the other.
 *
 * The assertion is deliberately geometric rather than count-based: on the frame
 * the tooltip disappears, some reported rect must reach into the band below the
 * anchor, because that is where the ghost of the dismissed tooltip still is. */
static void test_tooltip_dismissal_damages_the_below_anchor_band(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    /* Settle the widget, then hover it so the box's `tooltip` string arms. The
     * pointer is inside the label's rect, which is at the display origin. */
    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui,
                        &(optics_ui_label_opts){.text = "hover me",
                                                .box = {.id = "target", .tooltip = "byte counts"}});
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }

    optics_ui_input hover = IN0;
    hover.cursor = (optics_gfx_point){4.0f, 4.0f};
    optics_ui_begin(ui, &hover);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "hover me",
                                                .box = {.id = "target", .tooltip = "byte counts"}});
    optics_ui_test_end(ui);
    render_frame(ui, canvas);

    /* Dismiss: the pointer leaves, so the tooltip disappears and
     * `prev_tooltip_active` carries the band it vacated. */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "hover me",
                                                .box = {.id = "target", .tooltip = "byte counts"}});
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count > 0);
    /* The anchor is the label's rect (y == 0, height ~19) and the painted box
     * starts below it. At least one rect must reach past the anchor's bottom
     * edge, or the dismissed tooltip's band is never repainted. */
    bool below = false;
    for (uint32_t i = 0; i < count && !below; i++)
        below = rects[i].y + rects[i].h > 22.0f;
    OT_EXPECT(below);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* An icon blits a baked coverage mask with a halo covering half its stroke, the
 * outline twice over, and the AA fringe (render/icon_mask.c). Those pixels extend
 * past the command rect on every side. Damage that reported the rect alone left
 * the fringe frozen when a theme or hover change moved the icon — visible as a
 * ghost outline that survived until an unrelated full repaint. */
static void test_icon_damage_reaches_past_its_command_rect(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 200,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_icon(ui, &(optics_ui_icon_opts){
                               .id = OPTICS_UI_ICON_CHECK, .box = {.id = "ic"}, .size = 16.0f});
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }

    /* A colour change repaints the icon, halo included. */
    optics_ui_begin(ui, &IN0);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_CHECK,
                                              .box = {.id = "ic"},
                                              .size = 16.0f,
                                              .color = 0xFFFF0000u});
    optics_ui_test_end(ui);

    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count == 1);
    if (count == 1) {
        /* The command rect is 16x16; the halo is at least one pixel on each side
         * for a fill run and more for a stroked one. A reported width of exactly
         * 16 would mean the halo is not covered. */
        OT_EXPECT(rects[0].w > 16.0f);
        OT_EXPECT(rects[0].h > 16.0f);
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Caret blink is a phase, not a level (ADR-0151)                     */
/* ------------------------------------------------------------------ */

/* A focused text widget used to make every frame "needs repaint" for as long
 * as it held focus, because the query tested `caret_rect.w > 0`. That is a
 * constant, not a transition, and it is what made a host with any focused
 * field unable to quiesce (and made the compositor re-composite an identical
 * caret at display rate). The three properties below pin the replacement:
 * existence alone is not damage, a flip is, and settling is reachable. */
static char g_field_buf[64];

static void build_text_frame(optics_ui *ui, float dt) {
    optics_ui_input in = IN0;
    in.dt_seconds = dt;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "field"},
                                                      .buf = g_field_buf,
                                                      .cap = sizeof g_field_buf});
    optics_ui_test_end(ui);
}

/* Build a frame and *skip* presenting it — the state a damage-driven host is
 * in between a flip it has not yet painted and the frame that will paint it.
 * `test_end` would activate a snapshot and thereby consume the flip, which is
 * exactly what a skipped frame must not do. */
static void build_text_frame_unpresented(optics_ui *ui, float dt) {
    optics_ui_input in = IN0;
    in.dt_seconds = dt;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "field"},
                                                      .buf = g_field_buf,
                                                      .cap = sizeof g_field_buf});
    optics_ui_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));
}

/* Focus the text field by pressing inside it, then settle three frames so
 * "nothing changed" is the baseline the caret assertions start from. */
static void focus_text_field(optics_ui *ui, optics_gfx_canvas *canvas) {
    /* One build first: hit-testing reads the *presented* rect, which is zero
     * until a frame has been arranged. */
    build_text_frame(ui, 0.016f);
    render_frame(ui, canvas);
    optics_ui_input press = IN0;
    press.cursor = (optics_gfx_point){20, 14};
    press.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    press.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &press);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "field"},
                                                      .buf = g_field_buf,
                                                      .cap = sizeof g_field_buf});
    optics_ui_test_end(ui);
    render_frame(ui, canvas);
    for (int i = 0; i < 3; i++) {
        build_text_frame(ui, 0.016f);
        render_frame(ui, canvas);
    }
    OT_EXPECT(optics_ui_caret_rect(ui).w > 0.0f); /* there is a caret */
}

static void test_caret_existence_is_not_damage(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    /* Focus the field so a caret exists (a press inside its box), then let
     * the first frames settle. */
    focus_text_field(ui, canvas);

    /* Within one blink half-cycle nothing changes: a caret that merely exists
     * must not be damage. 0.5 s of 16 ms frames stays inside the 530 ms half
     * period, so no flip can have occurred. */
    bool repainted = false;
    for (int i = 0; i < 20; i++) {
        build_text_frame(ui, 0.016f);
        repainted |= optics_ui_frame_needs_repaint(ui);
        render_frame(ui, canvas);
    }
    OT_EXPECT(!repainted);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_caret_flip_is_damage_and_damages_only_its_rect(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    focus_text_field(ui, canvas);
    const bool presented = optics_ui_caret_visible(ui);
    OT_EXPECT(!optics_ui_caret_flip_pending(ui));

    /* Step past the half period in one frame and do NOT present it: the phase
     * must flip, and the flip must stay pending until a frame paints it. */
    build_text_frame_unpresented(ui, 0.6f);
    OT_EXPECT(optics_ui_caret_visible(ui) != presented);
    OT_EXPECT(optics_ui_caret_flip_pending(ui));
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));

    /* The damage must be the caret's own box, not the field or the viewport.
     * The blink changes two pixels of accent, so a full-surface repaint here
     * is exactly the amplification this ADR removes. */
    uint32_t count = 0;
    const optics_gfx_rect *rects = damage_rects(ui, &count);
    OT_EXPECT(count >= 1);
    const optics_gfx_rect caret = optics_ui_caret_rect(ui);
    bool covered = false;
    for (uint32_t i = 0; i < count; i++)
        covered |= rects[i].x <= caret.x && rects[i].y <= caret.y &&
                   rects[i].x + rects[i].w >= caret.x + caret.w &&
                   rects[i].y + rects[i].h >= caret.y + caret.h;
    OT_EXPECT(covered);
    /* Localization, not merely coverage: a blink must not cost the whole
     * surface. The field may span the page width, so the bound that matters is
     * the damaged *area* against the viewport, not a single rect's width. */
    double area = 0.0;
    for (uint32_t i = 0; i < count; i++)
        area += (double)rects[i].w * (double)rects[i].h;
    OT_EXPECT(area < 0.5 * 400.0 * 200.0);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_caret_clock_live_tracks_focus_and_switch(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    build_text_frame(ui, 0.016f);
    OT_EXPECT(!optics_ui_caret_clock_live(ui)); /* nothing focused: no clock owed */

    /* A press inside the field's box focuses it; one plain build first so
     * hit-testing has an arranged rect to test against. */
    build_text_frame(ui, 0.016f);
    optics_ui_input press = IN0;
    press.cursor = (optics_gfx_point){20, 14};
    press.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    press.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &press);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "field"},
                                                      .buf = g_field_buf,
                                                      .cap = sizeof g_field_buf});
    optics_ui_test_end(ui);
    build_text_frame(ui, 0.016f);
    OT_EXPECT(optics_ui_caret_clock_live(ui));

    /* A disabled blink is a solid caret: no clock, and therefore no idle tick
     * on a host that would otherwise quiesce. */
    optics_ui_set_caret_blink(ui, 0.0f);
    build_text_frame(ui, 0.016f);
    OT_EXPECT(!optics_ui_caret_clock_live(ui));
    OT_EXPECT(optics_ui_caret_visible(ui));

    optics_ui_release(ui);
}

static void test_host_owned_caret_reports_no_damage_or_clock(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);

    /* An app-owned editing surface (ADR-0151): the app paints its own caret
     * with its own clock, and lens only records the geometry so the platform
     * layer can place the IME candidate window. Lens must therefore report
     * neither damage nor a clock for it — otherwise two clocks drive one
     * caret, and lens manufactures flips the app's painter never made. */
    for (int i = 0; i < 3; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
        optics_ui_test_end(ui);
        render_frame(ui, canvas);
    }

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
    optics_ui_set_caret_rect(ui, (optics_gfx_rect){10, 10, 2, 18});
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_caret_rect(ui).w > 0.0f); /* geometry is still available */
    OT_EXPECT(!optics_ui_caret_clock_live(ui));
    OT_EXPECT(!optics_ui_caret_flip_pending(ui));

    /* Stepping well past a blink half-cycle must not make lens claim a flip:
     * the phase is not lens's to advance for this caret. */
    optics_ui_input slow = IN0;
    slow.dt_seconds = 0.6f;
    optics_ui_begin(ui, &slow);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "status"});
    optics_ui_set_caret_rect(ui, (optics_gfx_rect){10, 10, 2, 18});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_caret_flip_pending(ui));

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

static void test_host_caret_does_not_keep_an_unfocused_context_awake(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* No widget holds focus, so nothing in lens paints: a host-reported caret
     * rect alone must leave the context quiescent. */
    for (int i = 0; i < 4; i++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_set_caret_rect(ui, (optics_gfx_rect){10, 10, 2, 18});
        optics_ui_test_end(ui);
    }
    OT_EXPECT(!optics_ui_caret_clock_live(ui));
    OT_EXPECT(!optics_ui_frame_needs_repaint(ui));

    optics_ui_release(ui);
}

int main(void) {
    test_null_safety();
    test_first_frames_repaint_then_settle();
    test_leaf_change_repaints();
    test_place_open_close_repaints();
    test_damage_rect_bounds();
    test_placed_band_damage_rect_bounds();
    test_disjoint_bands_stay_disjoint();
    test_the_list_is_strictly_more_informative_than_its_union();
    test_flow_leaf_reports_its_glyphs_not_its_column();
    test_count_only_query_matches_the_written_list();
    test_centered_text_damage_follows_the_glyphs();
    test_overflow_collapses_to_one_bounding_box();
    test_damage_rects_null_safety();
    test_removed_child_repaints_its_vacated_band();
    test_removed_container_repaints_its_children();
    test_departed_damage_is_independent_of_unrelated_content();
    test_tooltip_dismissal_damages_the_below_anchor_band();
    test_icon_damage_reaches_past_its_command_rect();
    test_caret_existence_is_not_damage();
    test_caret_flip_is_damage_and_damages_only_its_rect();
    test_caret_clock_live_tracks_focus_and_switch();
    test_host_owned_caret_reports_no_damage_or_clock();
    test_host_caret_does_not_keep_an_unfocused_context_awake();
    return OT_REPORT();
}
