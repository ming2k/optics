/* test_repaint.c — lens_frame_needs_repaint: damage-driven repaint query. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <stdio.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Roomier surface for the removed-subtree tests: a departed band must sit in a
 * region no surviving widget happens to overlap. */
static const lens_input WIDE_IN = {.display_size = {900, 400}, .dt_seconds = 0.016f};

/* Two placed bands at opposite edges: the shape of a status strip plus a dock.
 * Their union is the whole viewport, which is why they must stay separate. */
static const lens_place_opts TOP_BAND = {
    .box = {.id = "top-band"},
    .band = LENS_BAND_CHROME,
    .mode = LENS_PLACE_EXACT,
    .rect = {0, 0, 400, 24},
    .layout = {.box = {.min_width = 400, .min_height = 24}, .pad = 2},
};

static const lens_place_opts BOTTOM_BAND = {
    .box = {.id = "bottom-band"},
    .band = LENS_BAND_CHROME,
    .mode = LENS_PLACE_EXACT,
    .rect = {0, 160, 400, 40},
    .layout = {.box = {.min_width = 400, .min_height = 40}, .pad = 2},
};

static void build_frame(lens *ui, float value) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)value);
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "status"});
    lens_label(ui, &(lens_label_opts){.text = val_str, .box = {.id = "val_node"}});
    test_end(ui);
}

static void render_frame(lens *ui, flux_canvas *canvas) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                             .clear_color = &clear}) == FLUX_OK);
    CHECK(test_snapshot_render(ui, canvas) == FLUX_OK);
    CHECK(flux_canvas_end(canvas) == FLUX_OK);
}

static bool rect_covers(flux_rect outer, flux_rect inner) {
    return outer.x <= inner.x && outer.y <= inner.y && outer.x + outer.w >= inner.x + inner.w &&
           outer.y + outer.h >= inner.y + inner.h;
}

/* The frame's damage list, via the public count-then-fill contract. Returns a
 * buffer with static lifetime so callers can inspect it after the helper. */
static const flux_rect *damage_rects(const lens *ui, uint32_t *count) {
    static flux_rect buffer[LENS_DAMAGE_RECTS_CAPACITY];
    *count = lens_damage_rects(ui, &(lens_damage_opts){
                                       .rects = buffer,
                                       .capacity = LENS_DAMAGE_RECTS_CAPACITY,
                                   });
    return buffer;
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
    CHECK(lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    CHECK(!lens_frame_needs_repaint(ui));

    flux_canvas_release(canvas);
    lens_release(ui);
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

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_place_open_close_repaints(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    const lens_place_opts popup = {
        .box = {.id = "menu"},
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_ANCHORED,
        .rect = {10, 10, 50, 20},
        .transient = true,
        .layout = {.box = {.min_width = 80}, .pad = 4},
    };

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    CHECK(!lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Open a transient popup: the tree's child set changes */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "status"});
    lens_label(ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    lens_place_open(ui, "menu");
    if (lens_place_begin(ui, &popup)) {
        lens_label(ui, &(lens_label_opts){.text = "item"});
        lens_place_end(ui);
    }
    test_end(ui);
    CHECK(lens_frame_needs_repaint(ui));
    render_frame(ui, canvas);

    /* Let the popup settle */
    for (int i = 0; i < 3; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, &(lens_label_opts){.text = "status"});
        lens_label(ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
        if (lens_place_begin(ui, &popup)) {
            lens_label(ui, &(lens_label_opts){.text = "item"});
            lens_place_end(ui);
        }
        test_end(ui);
        render_frame(ui, canvas);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    /* Close it: the popup's pixels must be erased. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "status"});
    lens_label(ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    lens_place_close(ui, "menu");
    if (lens_place_begin(ui, &popup)) {
        lens_label(ui, &(lens_label_opts){.text = "item"});
        lens_place_end(ui);
    }
    test_end(ui);
    CHECK(lens_frame_needs_repaint(ui));

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_damage_rect_bounds(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    /* Before anything is built there is nothing to describe. */
    uint32_t count = 0;
    CHECK(damage_rects(ui, &count) == NULL || count == 0);
    CHECK(lens_damage_rects(ui, &(lens_damage_opts){}) == 0);

    build_frame(ui, 0.5f);
    CHECK(lens_frame_needs_repaint(ui));
    const flux_rect *first = damage_rects(ui, &count);
    CHECK(count > 0);
    CHECK(first[0].w > 0.0f && first[0].h > 0.0f);
    render_frame(ui, canvas);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    CHECK(!lens_frame_needs_repaint(ui));
    CHECK(lens_damage_rects(ui, &(lens_damage_opts){}) == 0);
    render_frame(ui, canvas);

    /* Mutate leaf: every reported region must be inside the viewport. */
    build_frame(ui, 1.25f);
    CHECK(lens_frame_needs_repaint(ui));
    const flux_rect *dirty = damage_rects(ui, &count);
    CHECK(count > 0);
    for (uint32_t i = 0; i < count; i++) {
        CHECK(dirty[i].w > 0.0f && dirty[i].h > 0.0f);
        CHECK(dirty[i].x >= 0.0f && dirty[i].y >= 0.0f);
        CHECK(dirty[i].x + dirty[i].w <= 400.0f);
        CHECK(dirty[i].y + dirty[i].h <= 200.0f);
    }

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_placed_band_damage_rect_bounds(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(1920, 1080, 1.0f, &canvas) == FLUX_OK);

    const lens_place_opts chrome = {
        .box = {.id = "dock"},
        .band = LENS_BAND_CHROME,
        .mode = LENS_PLACE_ANCHORED,
        .rect = {0, 950, 500, 60},
        .layout = {.box = {.min_width = 500, .min_height = 60}, .pad = 4},
    };

    /* Build first frame and settle */
    lens_begin(ui, &IN0);
    if (lens_place_begin(ui, &chrome)) {
        lens_label(ui, &(lens_label_opts){.text = "item"});
        lens_place_end(ui);
    }
    test_end(ui);
    render_frame(ui, canvas);

    for (int i = 0; i < 3; i++) {
        lens_begin(ui, &IN0);
        if (lens_place_begin(ui, &chrome)) {
            lens_label(ui, &(lens_label_opts){.text = "item"});
            lens_place_end(ui);
        }
        test_end(ui);
        render_frame(ui, canvas);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    /* Mutate the placed chrome node label */
    lens_begin(ui, &IN0);
    if (lens_place_begin(ui, &chrome)) {
        lens_label(ui, &(lens_label_opts){.text = "modified"});
        lens_place_end(ui);
    }
    test_end(ui);

    CHECK(lens_frame_needs_repaint(ui));
    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 1);
    CHECK(rects[0].w > 0.0f && rects[0].h > 0.0f);
    /* Damage must stay inside the placed chrome band, NOT the full screen
     * (400x200). The band resolves to y = 140 after the ANCHORED flip and the
     * viewport clamp; the label that changed paints inside it. */
    CHECK(rects[0].w <= 400.0f);
    CHECK(rects[0].h <= 60.0f);
    CHECK(rects[0].y >= 140.0f);
    CHECK(rects[0].y + rects[0].h <= 200.0f);

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* A frame may change several unrelated regions at once. Collapsing them into one
 * bounding box is what makes a status tick and a dock spring re-composite the
 * whole output, so the list must stay disjoint (ADR-0123). */
static void test_disjoint_bands_stay_disjoint(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    uint32_t count = 0;

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        if (lens_place_begin(ui, &TOP_BAND)) {
            lens_label(ui, &(lens_label_opts){.text = "top"});
            lens_place_end(ui);
        }
        if (lens_place_begin(ui, &BOTTOM_BAND)) {
            lens_label(ui, &(lens_label_opts){.text = "bottom"});
            lens_place_end(ui);
        }
        test_end(ui);
        render_frame(ui, canvas);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    /* Touch both bands. */
    lens_begin(ui, &IN0);
    if (lens_place_begin(ui, &TOP_BAND)) {
        lens_label(ui, &(lens_label_opts){.text = "top!"});
        lens_place_end(ui);
    }
    if (lens_place_begin(ui, &BOTTOM_BAND)) {
        lens_label(ui, &(lens_label_opts){.text = "bottom!"});
        lens_place_end(ui);
    }
    test_end(ui);

    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 2);
    if (count == 2) {
        /* Sorted top-left first, and the two never merge: the union would be the
         * full 200 px height the bands are trying to avoid. */
        CHECK(rects[0].y < rects[1].y);
        CHECK(rects[0].y + rects[0].h <= rects[1].y);
        /* The two need not overlap in y, so a single-rect consumer would have to
         * span the gap between them. */
        CHECK(rects[0].h + rects[1].h < 200.0f);
    }

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* A host whose presentation API takes one rectangle must collapse the list
 * itself. This test pins the property that makes that collapse a deliberate,
 * local decision: the list is strictly more informative than its union, so
 * collapsing inside the producer would destroy information no consumer could
 * recover. */
static void test_the_list_is_strictly_more_informative_than_its_union(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        if (lens_place_begin(ui, &TOP_BAND)) {
            lens_label(ui, &(lens_label_opts){.text = "top"});
            lens_place_end(ui);
        }
        if (lens_place_begin(ui, &BOTTOM_BAND)) {
            lens_label(ui, &(lens_label_opts){.text = "bottom"});
            lens_place_end(ui);
        }
        test_end(ui);
        render_frame(ui, canvas);
    }

    lens_begin(ui, &IN0);
    if (lens_place_begin(ui, &TOP_BAND)) {
        lens_label(ui, &(lens_label_opts){.text = "top!"});
        lens_place_end(ui);
    }
    if (lens_place_begin(ui, &BOTTOM_BAND)) {
        lens_label(ui, &(lens_label_opts){.text = "bottom!"});
        lens_place_end(ui);
    }
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 2);
    flux_rect collapsed = {0, 0, 0, 0};
    int64_t covered = 0;
    for (uint32_t i = 0; i < count; i++) {
        collapsed = flux_rect_union(collapsed, rects[i]);
        covered += (int64_t)rects[i].w * (int64_t)rects[i].h;
    }
    const int64_t boxed = (int64_t)collapsed.w * (int64_t)collapsed.h;
    CHECK(boxed > covered * 4);

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* An edited widget reports its own paint, not the layout box it was arranged
 * into. A label laid out across a stretch column must not claim the column: the
 * column's rect is the whole display, and that is exactly the over-report that
 * turned one terminal cell into a full-output composite. */
static void test_flow_leaf_reports_its_glyphs_not_its_column(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(1920, 1080, 1.0f, &canvas) == FLUX_OK);

    static const lens_input BIG = {.display_size = {1920, 1080}, .dt_seconds = 0.016f};
    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &BIG);
        lens_label(ui, &(lens_label_opts){.text = "cell"});
        test_end(ui);
        flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
        CHECK(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &clear}) ==
              FLUX_OK);
        CHECK(test_snapshot_render(ui, canvas) == FLUX_OK);
        CHECK(flux_canvas_end(canvas) == FLUX_OK);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    lens_begin(ui, &BIG);
    lens_label(ui, &(lens_label_opts){.text = "celu"});
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(rects[0].w > 0.0f);
        CHECK(rects[0].w < 200.0f);
        CHECK(rects[0].h < 60.0f);
    }

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* Centred text paints at the middle of its box, not at its corner. A damage rect
 * taken from the box corner (or from the raw `rel` rectangle) reports pixels the
 * glyphs never touch and omits the ones they do — a stale-pixel bug that a
 * left-aligned-only test suite cannot see.
 *
 * This is the regression test for exactly that: the first implementation ignored
 * the `rel.w < 0` centring sentinel and reported the run at x = 0. */
static void test_centered_text_damage_follows_the_glyphs(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    const lens_label_opts centred = {
        .text = "AB",
        .align = LENS_CENTER,
        .box = {.id = "centred", .min_width = 380.0f},
    };

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, &centred);
        test_end(ui);
        render_frame(ui, canvas);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "AC",
                                      .align = LENS_CENTER,
                                      .box = {.id = "centred", .min_width = 380.0f}});
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 1);
    if (count == 1) {
        /* The run is centred in the 380px box, so its left edge is far from 0. */
        CHECK(rects[0].x > 100.0f);
        CHECK(rects[0].x + rects[0].w < 300.0f);
        CHECK(rects[0].w > 0.0f);
        CHECK(rects[0].w < 100.0f);
    }

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* A caller sizes its buffer from the count-only query, then fills it. Both
 * answers come from the same walk and must agree. */
static void test_count_only_query_matches_the_written_list(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.5f);
    render_frame(ui, canvas);
    build_frame(ui, 0.75f);

    const uint32_t asked = lens_damage_rects(ui, &(lens_damage_opts){.rects = NULL, .capacity = 0});
    CHECK(asked > 0);
    flux_rect buffer[LENS_DAMAGE_RECTS_CAPACITY];
    const uint32_t written = lens_damage_rects(ui, &(lens_damage_opts){
                                                       .rects = buffer,
                                                       .capacity = LENS_DAMAGE_RECTS_CAPACITY,
                                                   });
    CHECK(written == asked);

    /* A buffer too small reports the whole set as one box rather than a
     * truncated list: a dropped region leaves stale pixels on screen. */
    flux_rect one[1];
    const uint32_t trimmed = lens_damage_rects(ui, &(lens_damage_opts){
                                                       .rects = one,
                                                       .capacity = 1,
                                                   });
    CHECK(trimmed == 1);
    CHECK(one[0].w > 0.0f && one[0].h > 0.0f);

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* More distinct regions than the public contract carries must degrade to one
 * bounding box, never to a truncated list and never by reading past the fixed
 * accumulation buffer.
 *
 * This is the regression test for a bounds bug in the first implementation:
 * the accumulator counted rectangles it could not store and the normalizer was
 * handed that larger count, so it read uninitialized stack memory. */
static void test_overflow_collapses_to_one_bounding_box(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    /* Wide enough for more than LENS_DAMAGE_RECTS_CAPACITY disjoint leaves. */
    const uint32_t columns = LENS_DAMAGE_RECTS_CAPACITY + 16;
    CHECK(flux_canvas_create_cpu((float)columns * 8.0f, 200.0f, 1.0f, &canvas) == FLUX_OK);

    static lens_input wide = {.display_size = {0, 200}, .dt_seconds = 0.016f};
    wide.display_size.x = (float)columns * 8.0f;

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &wide);
        lens_row_begin(ui, &(lens_layout_opts){.gap = 0.0f});
        for (uint32_t c = 0; c < columns; c++) {
            /* Distinct ids: retained ids name nodes one-to-one, so a repeated
             * literal would collapse the whole row into a single node. */
            lens_push_id_int(ui, (int64_t)c);
            lens_label(ui, &(lens_label_opts){.text = "x", .box = {.id = "cell"}});
            lens_pop_id(ui);
        }
        lens_row_end(ui);
        test_end(ui);
        flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
        CHECK(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &clear}) ==
              FLUX_OK);
        CHECK(test_snapshot_render(ui, canvas) == FLUX_OK);
        CHECK(flux_canvas_end(canvas) == FLUX_OK);
    }
    CHECK(!lens_frame_needs_repaint(ui));

    /* Change every cell so the region count exceeds the contract. */
    lens_begin(ui, &wide);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 0.0f});
    for (uint32_t c = 0; c < columns; c++) {
        lens_push_id_int(ui, (int64_t)c);
        lens_label(ui, &(lens_label_opts){.text = "y", .box = {.id = "cell"}});
        lens_pop_id(ui);
    }
    lens_row_end(ui);
    test_end(ui);

    flux_rect buffer[LENS_DAMAGE_RECTS_CAPACITY];
    for (uint32_t i = 0; i < LENS_DAMAGE_RECTS_CAPACITY; i++)
        buffer[i] = (flux_rect){-1.0f, -1.0f, -1.0f, -1.0f};

    const uint32_t count = lens_damage_rects(ui, &(lens_damage_opts){
                                                     .rects = buffer,
                                                     .capacity = LENS_DAMAGE_RECTS_CAPACITY,
                                                 });
    /* Never more rectangles than the caller's capacity. */
    CHECK(count <= LENS_DAMAGE_RECTS_CAPACITY);
    CHECK(count > 0);
    /* Every slot the function claims to have written must hold a real rect. */
    for (uint32_t i = 0; i < count; i++) {
        CHECK(buffer[i].w > 0.0f);
        CHECK(buffer[i].h > 0.0f);
        CHECK(buffer[i].x + buffer[i].w <= wide.display_size.x);
    }
    /* Overflow is reported as one full-viewport rect: the omitted regions are
     * unknown, so a list or box built only from the survivors could under-cover
     * them. 65+ disjoint cells cannot fit the 64-rect contract, so this frame
     * must collapse. */
    CHECK(count == 1);
    CHECK_NEAR(buffer[0].x, 0.0f, 0.01f);
    CHECK_NEAR(buffer[0].y, 0.0f, 0.01f);
    CHECK_NEAR(buffer[0].w, wide.display_size.x, 0.01f);
    CHECK_NEAR(buffer[0].h, wide.display_size.y, 0.01f);

    /* A one-rect buffer must likewise answer with a single covering box. */
    flux_rect one[1] = {{-1.0f, -1.0f, -1.0f, -1.0f}};
    const uint32_t trimmed = lens_damage_rects(ui, &(lens_damage_opts){
                                                       .rects = one,
                                                       .capacity = 1,
                                                   });
    CHECK(trimmed == 1);
    CHECK(one[0].w > 0.0f && one[0].h > 0.0f);

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_damage_rects_null_safety(void) {
    CHECK(lens_damage_rects(NULL, NULL) == 0);
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    /* No frame built yet: no damage, and a null opts must not dereference. */
    CHECK(lens_damage_rects(ui, NULL) == 0);
    lens_release(ui);
}

/* A node removed from a FLOW container leaves pixels no other widget covers.
 *
 * The node is detached from its parent's child list immediately but stays in
 * the store for the leave grace window, so a damage walk over the tree cannot
 * reach it. Only a store sweep finds it. Regression for the region that stayed
 * frozen until some unrelated full repaint happened to cover it. */
static void test_removed_child_repaints_its_vacated_band(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    char tick[32];
    flux_rect row_c = {0};

    /* Settle a three-row list and capture the row's real bounds. The exact
     * glyph advance depends on the host's font build, so a hard-coded rect
     * would fail on a different FreeType/HarfBuzz version for reasons that
     * have nothing to do with damage. */
    for (int i = 0; i < 4; i++) {
        snprintf(tick, sizeof tick, "tick=%d", i);
        lens_begin(ui, &IN0);
        lens_label(ui, &(lens_label_opts){.text = tick, .box = {.id = "ticker"}});
        lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "list", .min_width = 120}});
        lens_label(ui, &(lens_label_opts){.text = "row-a"});
        lens_label(ui, &(lens_label_opts){.text = "row-b"});
        lens_response row_c_node = lens_label(ui, &(lens_label_opts){.text = "row-c"});
        lens_column_end(ui);
        test_end(ui);
        if (i == 3) {
            CHECK(row_c_node.id != 0);
            row_c = lens_node_bounds(lens_find(ui, row_c_node.id));
        }
        render_frame(ui, canvas);
    }
    CHECK(row_c.w > 0.0f && row_c.h > 0.0f);

    /* Drop the trailing row. The ticker changes in the same frame, so the
     * "no region named" viewport fallback cannot mask an omission here. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "tick=9", .box = {.id = "ticker"}});
    lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "list", .min_width = 120}});
    lens_label(ui, &(lens_label_opts){.text = "row-a"});
    lens_label(ui, &(lens_label_opts){.text = "row-b"});
    lens_column_end(ui);
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count > 0);

    /* The vacated band must be covered by at least one reported rect. */
    bool covered = false;
    for (uint32_t i = 0; i < count && !covered; i++)
        covered = rect_covers(rects[i], row_c);
    CHECK(covered);

    /* Localization holds: a couple of small strips, not the whole display the
     * pre-ADR-0143 fallback would have reported. */
    CHECK(count <= 4);
    for (uint32_t i = 0; i < count; i++)
        CHECK(rects[i].w < 200.0f);

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* A removed subtree is erased through its children: the departed container
 * paints nothing itself, so its own bounds are not where the pixels are. */
static void test_removed_container_repaints_its_children(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, &(lens_label_opts){.text = "header", .box = {.id = "header"}});
        lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "panel"}});
        lens_label(ui, &(lens_label_opts){.text = "inner-text"});
        lens_column_end(ui);
        test_end(ui);
        render_frame(ui, canvas);
    }

    /* Remove the whole panel; the header changes with it. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "header2", .box = {.id = "header"}});
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count > 0);
    /* `inner-text` is laid out below the header, so some rect must reach past
     * the header's band. */
    bool below = false;
    for (uint32_t i = 0; i < count && !below; i++)
        below = rects[i].y + rects[i].h > 20.0f;
    CHECK(below);

    flux_canvas_release(canvas);
    lens_release(ui);
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
    flux_rect expect = {0};
    bool have_expect = false;
    for (int fillers = 0; fillers <= 40; fillers = fillers ? fillers * 8 : 1) {
        lens *ui = NULL;
        flux_canvas *canvas = NULL;
        CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
        CHECK(flux_canvas_create_cpu(900, 400, 1.0f, &canvas) == FLUX_OK);

        flux_rect row_c = {0};
        for (int i = 0; i < 4; i++) {
            lens_begin(ui, &WIDE_IN);
            lens_label(ui, &(lens_label_opts){.text = "ticker", .box = {.id = "ticker"}});
            lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "list", .min_width = 120}});
            lens_label(ui, &(lens_label_opts){.text = "row-a"});
            lens_label(ui, &(lens_label_opts){.text = "row-b"});
            lens_response row_c_node = lens_label(ui, &(lens_label_opts){.text = "row-c"});
            lens_column_end(ui);
            test_end(ui);
            if (i == 3) {
                CHECK(row_c_node.id != 0);
                row_c = lens_node_bounds(lens_find(ui, row_c_node.id));
            }
            render_frame(ui, canvas);
        }
        CHECK(row_c.w > 0.0f && row_c.h > 0.0f);

        lens_begin(ui, &WIDE_IN);
        lens_label(ui, &(lens_label_opts){.text = "tick=9", .box = {.id = "ticker"}});
        lens_column_begin(ui, &(lens_layout_opts){.box = {.id = "list", .min_width = 120}});
        lens_label(ui, &(lens_label_opts){.text = "row-a"});
        lens_label(ui, &(lens_label_opts){.text = "row-b"});
        lens_column_end(ui);
        for (int i = 0; i < fillers; i++)
            lens_label(ui, &(lens_label_opts){.text = "the quick brown fox jumps over 0123456789",
                                              .box = {.id = "filler"}});
        test_end(ui);

        uint32_t count = 0;
        const flux_rect *rects = damage_rects(ui, &count);
        flux_rect departed = {0};
        for (uint32_t i = 0; i < count && departed.w == 0.0f; i++)
            if (rect_covers(rects[i], row_c))
                departed = rects[i];
        CHECK(departed.w > 0.0f && departed.h > 0.0f);

        /* The first iteration defines the expected region; unrelated content
         * must not move or resize it on later iterations. */
        if (!have_expect) {
            expect = departed;
            have_expect = true;
        } else {
            CHECK_NEAR(departed.x, expect.x, 0.01f);
            CHECK_NEAR(departed.y, expect.y, 0.01f);
            CHECK_NEAR(departed.w, expect.w, 0.01f);
            CHECK_NEAR(departed.h, expect.h, 0.01f);
        }

        flux_canvas_release(canvas);
        lens_release(ui);
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
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    /* Settle the widget, then hover it so the box's `tooltip` string arms. The
     * pointer is inside the label's rect, which is at the display origin. */
    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, &(lens_label_opts){.text = "hover me",
                                          .box = {.id = "target", .tooltip = "byte counts"}});
        test_end(ui);
        render_frame(ui, canvas);
    }

    lens_input hover = IN0;
    hover.cursor = (flux_point){4.0f, 4.0f};
    lens_begin(ui, &hover);
    lens_label(ui, &(lens_label_opts){.text = "hover me",
                                      .box = {.id = "target", .tooltip = "byte counts"}});
    test_end(ui);
    render_frame(ui, canvas);

    /* Dismiss: the pointer leaves, so the tooltip disappears and
     * `prev_tooltip_active` carries the band it vacated. */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "hover me",
                                      .box = {.id = "target", .tooltip = "byte counts"}});
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count > 0);
    /* The anchor is the label's rect (y == 0, height ~19) and the painted box
     * starts below it. At least one rect must reach past the anchor's bottom
     * edge, or the dismissed tooltip's band is never repainted. */
    bool below = false;
    for (uint32_t i = 0; i < count && !below; i++)
        below = rects[i].y + rects[i].h > 22.0f;
    CHECK(below);

    flux_canvas_release(canvas);
    lens_release(ui);
}

/* An icon blits a baked coverage mask with a halo covering half its stroke, the
 * outline twice over, and the AA fringe (render/icon_mask.c). Those pixels extend
 * past the command rect on every side. Damage that reported the rect alone left
 * the fringe frozen when a theme or hover change moved the icon — visible as a
 * ghost outline that survived until an unrelated full repaint. */
static void test_icon_damage_reaches_past_its_command_rect(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(200, 200, 1.0f, &canvas) == FLUX_OK);

    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        lens_icon(ui, &(lens_icon_opts){.id = LENS_ICON_CHECK, .box = {.id = "ic"}, .size = 16.0f});
        test_end(ui);
        render_frame(ui, canvas);
    }

    /* A colour change repaints the icon, halo included. */
    lens_begin(ui, &IN0);
    lens_icon(ui,
              &(lens_icon_opts){
                  .id = LENS_ICON_CHECK, .box = {.id = "ic"}, .size = 16.0f, .color = 0xFFFF0000u});
    test_end(ui);

    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count == 1);
    if (count == 1) {
        /* The command rect is 16x16; the halo is at least one pixel on each side
         * for a fill run and more for a stroked one. A reported width of exactly
         * 16 would mean the halo is not covered. */
        CHECK(rects[0].w > 16.0f);
        CHECK(rects[0].h > 16.0f);
    }

    flux_canvas_release(canvas);
    lens_release(ui);
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

static void build_text_frame(lens *ui, float dt) {
    lens_input in = IN0;
    in.dt_seconds = dt;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){
                          .box = {.id = "field"}, .buf = g_field_buf, .cap = sizeof g_field_buf});
    test_end(ui);
}

/* Build a frame and *skip* presenting it — the state a damage-driven host is
 * in between a flip it has not yet painted and the frame that will paint it.
 * `test_end` would activate a snapshot and thereby consume the flip, which is
 * exactly what a skipped frame must not do. */
static void build_text_frame_unpresented(lens *ui, float dt) {
    lens_input in = IN0;
    in.dt_seconds = dt;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){
                          .box = {.id = "field"}, .buf = g_field_buf, .cap = sizeof g_field_buf});
    lens_end(ui);
    CHECK(!lens_overflowed(ui));
}

/* Focus the text field by pressing inside it, then settle three frames so
 * "nothing changed" is the baseline the caret assertions start from. */
static void focus_text_field(lens *ui, flux_canvas *canvas) {
    /* One build first: hit-testing reads the *presented* rect, which is zero
     * until a frame has been arranged. */
    build_text_frame(ui, 0.016f);
    render_frame(ui, canvas);
    lens_input press = IN0;
    press.cursor = (flux_point){20, 14};
    press.mouse_pressed[LENS_MOUSE_LEFT] = true;
    press.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &press);
    lens_textedit(ui, &(lens_textedit_opts){
                          .box = {.id = "field"}, .buf = g_field_buf, .cap = sizeof g_field_buf});
    test_end(ui);
    render_frame(ui, canvas);
    for (int i = 0; i < 3; i++) {
        build_text_frame(ui, 0.016f);
        render_frame(ui, canvas);
    }
    CHECK(lens_caret_rect(ui).w > 0.0f); /* there is a caret */
}

static void test_caret_existence_is_not_damage(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    /* Focus the field so a caret exists (a press inside its box), then let
     * the first frames settle. */
    focus_text_field(ui, canvas);

    /* Within one blink half-cycle nothing changes: a caret that merely exists
     * must not be damage. 0.5 s of 16 ms frames stays inside the 530 ms half
     * period, so no flip can have occurred. */
    bool repainted = false;
    for (int i = 0; i < 20; i++) {
        build_text_frame(ui, 0.016f);
        repainted |= lens_frame_needs_repaint(ui);
        render_frame(ui, canvas);
    }
    CHECK(!repainted);

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_caret_flip_is_damage_and_damages_only_its_rect(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    focus_text_field(ui, canvas);
    const bool presented = lens_caret_visible(ui);
    CHECK(!lens_caret_flip_pending(ui));

    /* Step past the half period in one frame and do NOT present it: the phase
     * must flip, and the flip must stay pending until a frame paints it. */
    build_text_frame_unpresented(ui, 0.6f);
    CHECK(lens_caret_visible(ui) != presented);
    CHECK(lens_caret_flip_pending(ui));
    CHECK(lens_frame_needs_repaint(ui));

    /* The damage must be the caret's own box, not the field or the viewport.
     * The blink changes two pixels of accent, so a full-surface repaint here
     * is exactly the amplification this ADR removes. */
    uint32_t count = 0;
    const flux_rect *rects = damage_rects(ui, &count);
    CHECK(count >= 1);
    const flux_rect caret = lens_caret_rect(ui);
    bool covered = false;
    for (uint32_t i = 0; i < count; i++)
        covered |= rects[i].x <= caret.x && rects[i].y <= caret.y &&
                   rects[i].x + rects[i].w >= caret.x + caret.w &&
                   rects[i].y + rects[i].h >= caret.y + caret.h;
    CHECK(covered);
    /* Localization, not merely coverage: a blink must not cost the whole
     * surface. The field may span the page width, so the bound that matters is
     * the damaged *area* against the viewport, not a single rect's width. */
    double area = 0.0;
    for (uint32_t i = 0; i < count; i++)
        area += (double)rects[i].w * (double)rects[i].h;
    CHECK(area < 0.5 * 400.0 * 200.0);

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_caret_clock_live_tracks_focus_and_switch(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    build_text_frame(ui, 0.016f);
    CHECK(!lens_caret_clock_live(ui)); /* nothing focused: no clock owed */

    /* A press inside the field's box focuses it; one plain build first so
     * hit-testing has an arranged rect to test against. */
    build_text_frame(ui, 0.016f);
    lens_input press = IN0;
    press.cursor = (flux_point){20, 14};
    press.mouse_pressed[LENS_MOUSE_LEFT] = true;
    press.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &press);
    lens_textedit(ui, &(lens_textedit_opts){
                          .box = {.id = "field"}, .buf = g_field_buf, .cap = sizeof g_field_buf});
    test_end(ui);
    build_text_frame(ui, 0.016f);
    CHECK(lens_caret_clock_live(ui));

    /* A disabled blink is a solid caret: no clock, and therefore no idle tick
     * on a host that would otherwise quiesce. */
    lens_set_caret_blink(ui, 0.0f);
    build_text_frame(ui, 0.016f);
    CHECK(!lens_caret_clock_live(ui));
    CHECK(lens_caret_visible(ui));

    lens_release(ui);
}

static void test_host_owned_caret_reports_no_damage_or_clock(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 200, 1.0f, &canvas) == FLUX_OK);

    /* An app-owned editing surface (ADR-0151): the app paints its own caret
     * with its own clock, and lens only records the geometry so the platform
     * layer can place the IME candidate window. Lens must therefore report
     * neither damage nor a clock for it — otherwise two clocks drive one
     * caret, and lens manufactures flips the app's painter never made. */
    for (int i = 0; i < 3; i++) {
        lens_begin(ui, &IN0);
        lens_label(ui, &(lens_label_opts){.text = "status"});
        test_end(ui);
        render_frame(ui, canvas);
    }

    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "status"});
    lens_set_caret_rect(ui, (flux_rect){10, 10, 2, 18});
    test_end(ui);

    CHECK(lens_caret_rect(ui).w > 0.0f); /* geometry is still available */
    CHECK(!lens_caret_clock_live(ui));
    CHECK(!lens_caret_flip_pending(ui));

    /* Stepping well past a blink half-cycle must not make lens claim a flip:
     * the phase is not lens's to advance for this caret. */
    lens_input slow = IN0;
    slow.dt_seconds = 0.6f;
    lens_begin(ui, &slow);
    lens_label(ui, &(lens_label_opts){.text = "status"});
    lens_set_caret_rect(ui, (flux_rect){10, 10, 2, 18});
    test_end(ui);
    CHECK(!lens_caret_flip_pending(ui));

    flux_canvas_release(canvas);
    lens_release(ui);
}

static void test_host_caret_does_not_keep_an_unfocused_context_awake(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* No widget holds focus, so nothing in lens paints: a host-reported caret
     * rect alone must leave the context quiescent. */
    for (int i = 0; i < 4; i++) {
        lens_begin(ui, &IN0);
        lens_set_caret_rect(ui, (flux_rect){10, 10, 2, 18});
        test_end(ui);
    }
    CHECK(!lens_caret_clock_live(ui));
    CHECK(!lens_frame_needs_repaint(ui));

    lens_release(ui);
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
    return TEST_REPORT();
}
