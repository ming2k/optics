/*
 * test_rfc0094_contracts.c — Validation suite for RFC-0094 / ADR-0087:
 * - Dual-lifecycle contracts (_retain/_release, _init/_deinit/_reset)
 * - Target-driven canvas begin/end polymorphism
 * - FLUX_INIT safe initialization macro
 * - Zero-cost inline canvas helpers (<flux/canvas_helpers.h>)
 * - Font atlas explicit synchronization query
 */

#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_helpers.h>
#include <flux/core.h>
#include <flux/math.h>
#include <flux-text/text.h>

static void test_flux_init_macro(void) {
    /* Lowercase struct form */
    flux_buffer_desc b1 = FLUX_INIT(buffer_desc,
        .size = 1024,
        .usage = FLUX_BUFFER_USAGE_VERTEX,
        .location = FLUX_BUFFER_HOST_VISIBLE
    );
    EXPECT(b1.type == FLUX_TYPE_BUFFER_DESC);
    EXPECT(b1.next == nullptr);
    EXPECT(b1.size == 1024);
    EXPECT(b1.usage == FLUX_BUFFER_USAGE_VERTEX);

    /* Uppercase descriptor form */
    flux_target_desc t1 = FLUX_INIT(TARGET_DESC,
        .width = 800,
        .height = 600,
        .usage = FLUX_TARGET_COLOR,
        .format = FLUX_FORMAT_RGBA8_UNORM
    );
    EXPECT(t1.type == FLUX_TYPE_TARGET_DESC);
    EXPECT(t1.next == nullptr);
    EXPECT(t1.width == 800);
    EXPECT(t1.height == 600);

    /* CPU target desc */
    flux_cpu_target_desc ct1 = FLUX_INIT(CPU_TARGET_DESC,
        .width = 64,
        .height = 64,
        .format = FLUX_FORMAT_RGBA8_UNORM
    );
    EXPECT(ct1.type == FLUX_TYPE_TARGET_DESC);
    EXPECT(ct1.next == nullptr);
    EXPECT(ct1.width == 64);
}

static void test_arena_lifecycle(void) {
    flux_arena a;
    flux_result r = flux_arena_init(&a, 4096, nullptr);
    EXPECT(r == FLUX_OK);
    EXPECT(a.capacity == 4096);
    EXPECT(a.used == 0);

    void *p = flux_arena_alloc(&a, 128);
    EXPECT(p != nullptr);
    EXPECT(a.used >= 128);

    flux_arena_reset(&a);
    EXPECT(a.used == 0);

    /* Test path create on arena */
    flux_path *path = nullptr;
    r = flux_path_create(&path, &a);
    EXPECT(r == FLUX_OK);
    EXPECT(path != nullptr);
    flux_path_move_to(path, 0.0f, 0.0f);
    flux_path_line_to(path, 10.0f, 10.0f);
    flux_path_reset(path);

    /* Deinit */
    flux_arena_deinit(&a);
    EXPECT(a.base == nullptr);
}

static void test_cpu_target_and_canvas_polymorphism(void) {
    /* 1. Create CPU Target */
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC,
        .width = 64,
        .height = 64,
        .format = FLUX_FORMAT_RGBA8_UNORM
    );
    flux_target *target = nullptr;
    flux_result r = flux_target_create_cpu(&t_desc, &target);
    EXPECT(r == FLUX_OK);
    EXPECT(target != nullptr);
    EXPECT(flux_target_width(target) == 64);
    EXPECT(flux_target_height(target) == 64);

    /* Retain and release */
    flux_target *retained = flux_target_retain(target);
    EXPECT(retained == target);
    flux_target_release(retained);

    /* 2. Create CPU Canvas */
    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC,
        .backend = FLUX_CANVAS_BACKEND_CPU,
        .width = 64,
        .height = 64,
        .scale = 1.0f
    );
    flux_canvas *canvas = nullptr;
    r = flux_canvas_create(&c_desc, &canvas);
    EXPECT(r == FLUX_OK);
    EXPECT(canvas != nullptr);

    /* Canvas retain and release check */
    flux_canvas *c_retained = flux_canvas_retain(canvas);
    EXPECT(c_retained == canvas);
    flux_canvas_release(c_retained);

    /* 3. Polymorphic Target-Driven pass bracket */
    flux_color clear = flux_color_rgba_premul(10, 20, 30, 255);
    r = flux_canvas_begin(canvas, target, &clear);
    EXPECT(r == FLUX_OK);

    /* 4. Use inline canvas helpers from <flux/canvas_helpers.h> */
    flux_rect rect = { 4.0f, 4.0f, 24.0f, 24.0f };
    flux_color rect_color = flux_color_rgba_premul(255, 0, 0, 255);
    flux_canvas_fill_rect_color(canvas, rect, rect_color);

    flux_rect rrect = { 32.0f, 4.0f, 24.0f, 24.0f };
    flux_color rrect_color = flux_color_rgba_premul(0, 255, 0, 255);
    flux_canvas_fill_rrect(canvas, rrect, 4.0f, rrect_color);

    r = flux_canvas_end(canvas);
    EXPECT(r == FLUX_OK);

    /* 5. Extract pixels */
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = flux_target_cpu_pixels(target, &w, &h, &stride);
    EXPECT(px != nullptr);
    EXPECT(w == 64);
    EXPECT(h == 64);
    EXPECT(stride >= 64 * 4);

    /* 6. Release resources */
    flux_canvas_release(canvas);
    flux_target_release(target);
}

static void test_text_lifecycle_and_atlas_sync(void) {
    flux_text_desc desc = {
        .device = nullptr,
        .scale = 1.0f,
    };
    flux_text *text = nullptr;
    flux_result r = flux_text_create(&desc, &text);
    if (r == FLUX_OK && text) {
        /* Retain / release */
        flux_text *retained = flux_text_retain(text);
        EXPECT(retained == text);
        flux_text_release(retained);

        /* Query atlas sync */
        bool pending = flux_text_has_pending_uploads(text);
        EXPECT(!pending);

        flux_result fr = flux_text_flush_atlas(text, nullptr);
        EXPECT(fr == FLUX_OK);

        flux_text_release(text);
    }
}

static void test_adr0088_clean_break(void) {
    /* 1. Geometry Tagged Union size check */
    EXPECT(sizeof(flux_geometry) <= 48);

    /* 2. Brush and geometry construction */
    flux_brush solid_b = flux_brush_solid(flux_color_rgba_premul(255, 0, 0, 255));
    EXPECT(solid_b.kind == FLUX_BRUSH_SOLID);
    EXPECT(solid_b.opacity == 1.0f);

    flux_geometry r_geom = flux_geom_rect((flux_rect){10.0f, 10.0f, 50.0f, 50.0f});
    EXPECT(r_geom.kind == FLUX_GEOM_RECT);

    flux_geometry sq_geom = flux_geom_squircle((flux_rect){0.0f, 0.0f, 100.0f, 100.0f}, 16.0f, 0.8f);
    EXPECT(sq_geom.kind == FLUX_GEOM_SQUIRCLE);
    EXPECT(sq_geom.squircle.curvature == 0.8f);

    /* 3. Encoder and DisplayList pure CPU recording */
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 16384, nullptr) == FLUX_OK);

    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    EXPECT(enc != nullptr);

    flux_encoder_save(enc);
    flux_encoder_translate(enc, 10.0f, 10.0f);
    flux_encoder_draw_geometry(enc, &r_geom, &solid_b);
    flux_encoder_draw_geometry(enc, &sq_geom, &solid_b);
    flux_encoder_save_layer(enc, nullptr, 0.8f);
    flux_encoder_restore(enc);
    flux_encoder_restore(enc);

    flux_display_list *dl = nullptr;
    EXPECT(flux_encoder_finish(enc, &dl) == FLUX_OK);
    EXPECT(flux_display_list_command_count(dl) > 0);
    EXPECT(flux_display_list_size(dl) > 0);
    EXPECT(dl != nullptr);

    flux_encoder_destroy(enc);

    /* 4. Play back into CPU canvas via flux_canvas_submit_display_list */
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC,
        .width = 128,
        .height = 128,
        .format = FLUX_FORMAT_RGBA8_UNORM
    );
    flux_target *target = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &target) == FLUX_OK);

    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC,
        .backend = FLUX_CANVAS_BACKEND_CPU
    );
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c) == FLUX_OK);

    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, target, &clear) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, dl) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);

    flux_display_list_release(dl);
    flux_canvas_release(c);
    flux_target_release(target);
    flux_arena_deinit(&arena);
}

static void test_adr0090_display_list_immutable_capture(void) {
    flux_arena caller_arena;
    EXPECT(flux_arena_init(&caller_arena, 8192, nullptr) == FLUX_OK);

    /* Allocate path in caller's transient arena */
    flux_path *path = nullptr;
    EXPECT(flux_path_create(&path, &caller_arena) == FLUX_OK);
    flux_path_move_to(path, 10.0f, 10.0f);
    flux_path_line_to(path, 50.0f, 10.0f);
    flux_path_line_to(path, 50.0f, 50.0f);
    flux_path_close(path);

    /* Record into encoder */
    flux_arena enc_arena;
    EXPECT(flux_arena_init(&enc_arena, 8192, nullptr) == FLUX_OK);
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);

    flux_geometry p_geom = {.kind = FLUX_GEOM_PATH, .path = {.path = path}};
    flux_brush solid_b = flux_brush_solid(flux_color_rgba_premul(0, 255, 0, 255));
    flux_encoder_draw_geometry(enc, &p_geom, &solid_b);

    /* Finish into display list */
    flux_display_list *dl = nullptr;
    EXPECT(flux_encoder_finish(enc, &dl) == FLUX_OK);
    EXPECT(flux_display_list_command_count(dl) == 1);
    EXPECT(flux_display_list_size(dl) > 0);

    /* Now destroy encoder, reset encoder arena, and completely reset/destroy caller's path and arena */
    flux_encoder_destroy(enc);
    flux_arena_deinit(&enc_arena);
    flux_path_reset(path);
    flux_arena_deinit(&caller_arena);

    /* Play back display list into CPU canvas — the embedded path segments must survive! */
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC, .width = 64, .height = 64, .format = FLUX_FORMAT_RGBA8_UNORM);
    flux_target *target = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &target) == FLUX_OK);

    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU);
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c) == FLUX_OK);

    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, target, &clear) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, dl) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);

    /* Verify pixels at (30, 20) inside the triangle are rendered green */
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = flux_target_cpu_pixels(target, &w, &h, &stride);
    EXPECT(px != nullptr);
    const uint8_t *sample = &px[(20 * w + 30) * 4];
    EXPECT(sample[1] > 200); /* green channel */
    EXPECT(sample[3] > 200); /* alpha channel */

    flux_display_list_release(dl);
    flux_canvas_release(c);
    flux_target_release(target);
}

static void test_adr0091_save_layer_opacity_group(void) {
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC, .width = 64, .height = 64, .format = FLUX_FORMAT_RGBA8_UNORM);
    flux_target *target = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &target) == FLUX_OK);

    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU);
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c) == FLUX_OK);

    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, target, &clear) == FLUX_OK);

    /* Opacity Group with 0.5 opacity:
     * Draw two overlapping fully-opaque red rectangles.
     * Rect 1: [8, 8, 24, 24]
     * Rect 2: [20, 8, 24, 24]  -> overlap is [20..32, 8..32] */
    flux_canvas_save_layer(c, nullptr, 0.5f);
    flux_color red = flux_color_rgba_premul(255, 0, 0, 255);
    flux_canvas_fill_rect_color(c, (flux_rect){8.0f, 8.0f, 24.0f, 24.0f}, red);
    flux_canvas_fill_rect_color(c, (flux_rect){20.0f, 8.0f, 24.0f, 24.0f}, red);
    flux_canvas_restore(c);

    EXPECT(flux_canvas_end(c) == FLUX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = flux_target_cpu_pixels(target, &w, &h, &stride);
    EXPECT(px != nullptr);

    /* Non-overlapping pixel at (12, 16): Rect 1 only */
    const uint8_t *px_single = &px[(16 * w + 12) * 4];
    /* Overlapping pixel at (24, 16): both Rect 1 and Rect 2 */
    const uint8_t *px_overlap = &px[(16 * w + 24) * 4];

    /* In a true Opacity Group, both pixels MUST have identical alpha and color (~128),
     * NOT the double-blend artifact (191) of immediate opacity */
    EXPECT(abs((int)px_single[3] - 128) <= 4);
    EXPECT(abs((int)px_overlap[3] - 128) <= 4);
    EXPECT(abs((int)px_single[3] - (int)px_overlap[3]) <= 2);
    EXPECT(abs((int)px_single[0] - (int)px_overlap[0]) <= 2);

    flux_canvas_release(c);
    flux_target_release(target);
}

static void test_adr0091_squircle_distinct_geometry(void) {
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC, .width = 64, .height = 64, .format = FLUX_FORMAT_RGBA8_UNORM);
    flux_target *t_sq = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &t_sq) == FLUX_OK);

    flux_target *t_rr = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &t_rr) == FLUX_OK);

    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU);
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c) == FLUX_OK);

    flux_color clear = 0;
    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    flux_rect box = {4.0f, 4.0f, 56.0f, 56.0f};

    /* Draw squircle */
    EXPECT(flux_canvas_begin(c, t_sq, &clear) == FLUX_OK);
    flux_geometry g_sq = flux_geom_squircle(box, 16.0f, 1.0f);
    flux_brush b = flux_brush_solid(white);
    flux_canvas_draw_geometry(c, &g_sq, &b);
    EXPECT(flux_canvas_end(c) == FLUX_OK);

    /* Draw rrect */
    EXPECT(flux_canvas_begin(c, t_rr, &clear) == FLUX_OK);
    flux_geometry g_rr = flux_geom_rrect(box, 16.0f);
    flux_canvas_draw_geometry(c, &g_rr, &b);
    EXPECT(flux_canvas_end(c) == FLUX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px_sq = flux_target_cpu_pixels(t_sq, &w, &h, &stride);
    const uint8_t *px_rr = flux_target_cpu_pixels(t_rr, &w, &h, &stride);

    /* Squircle has distinct corner shoulder curvature compared to standard circular arc. */
    bool diff_detected = false;
    for (uint32_t y = 4; y < 20; y++) {
        for (uint32_t x = 4; x < 20; x++) {
            if (px_sq[(y * w + x) * 4 + 3] != px_rr[(y * w + x) * 4 + 3]) {
                diff_detected = true;
                break;
            }
        }
        if (diff_detected) break;
    }
    EXPECT(diff_detected);

    flux_canvas_release(c);
    flux_target_release(t_sq);
    flux_target_release(t_rr);
}

static void test_adr0092_text_atlas_epoch_tracking(void) {
    flux_text_desc desc = {.scale = 1.0f};
    flux_text *text = nullptr;
    EXPECT(flux_text_create(&desc, &text) == FLUX_OK);

    uint64_t epoch0 = flux_text_get_atlas_epoch(text, nullptr);
    EXPECT(flux_text_flush_atlas(text, nullptr) == FLUX_OK);
    uint64_t epoch1 = flux_text_get_atlas_epoch(text, nullptr);
    EXPECT(epoch1 > epoch0);

    flux_text_release(text);
}

static void test_adr0093_target_exclusive_borrow_and_readback(void) {
    flux_cpu_target_desc t_desc = FLUX_INIT(CPU_TARGET_DESC, .width = 32, .height = 32, .format = FLUX_FORMAT_RGBA8_UNORM);
    flux_target *target = nullptr;
    EXPECT(flux_target_create_cpu(&t_desc, &target) == FLUX_OK);

    flux_canvas_desc c_desc = FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU, .width = 32, .height = 32);
    flux_canvas *c1 = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c1) == FLUX_OK);
    flux_canvas *c2 = nullptr;
    EXPECT(flux_canvas_create(&c_desc, &c2) == FLUX_OK);

    /* 1. Begin on target with c1 */
    flux_color red = flux_color_rgba_premul(255, 0, 0, 255);
    EXPECT(flux_canvas_begin(c1, target, &red) == FLUX_OK);

    /* 2. Overlapping begin on the same target with c2 MUST fail with INVALID_STATE (ADR-0093) */
    EXPECT(flux_canvas_begin(c2, target, &red) == FLUX_ERROR_INVALID_STATE);

    /* 3. Readback of target pixels during active session MUST return nullptr */
    uint32_t w = 0, h = 0, stride = 0;
    EXPECT(flux_target_cpu_pixels(target, &w, &h, &stride) == nullptr);

    /* 4. Canvas readback while recording MUST return nullptr */
    EXPECT(flux_canvas_read_pixels(c1, &w, &h, &stride) == nullptr);

    /* 5. End session cleanly on c1 */
    EXPECT(flux_canvas_end(c1) == FLUX_OK);

    /* 6. Target readback is now safe and exposed */
    const uint8_t *px = flux_target_cpu_pixels(target, &w, &h, &stride);
    EXPECT(px != nullptr);
    EXPECT(px[0] == 255); /* Red clear color */

    /* 7. Target can now be bound to c2 */
    flux_color blue = flux_color_rgba_premul(0, 0, 255, 255);
    EXPECT(flux_canvas_begin(c2, target, &blue) == FLUX_OK);
    EXPECT(flux_canvas_end(c2) == FLUX_OK);
    px = flux_target_cpu_pixels(target, &w, &h, &stride);
    EXPECT(px != nullptr);
    EXPECT(px[2] == 255); /* Blue clear color */

    flux_canvas_release(c1);
    flux_canvas_release(c2);
    flux_target_release(target);
}

int main(void) {
    test_flux_init_macro();
    test_arena_lifecycle();
    test_cpu_target_and_canvas_polymorphism();
    test_text_lifecycle_and_atlas_sync();
    test_adr0088_clean_break();
    test_adr0090_display_list_immutable_capture();
    test_adr0091_save_layer_opacity_group();
    test_adr0091_squircle_distinct_geometry();
    test_adr0092_text_atlas_epoch_tracking();
    test_adr0093_target_exclusive_borrow_and_readback();

    TEST_SUMMARY();
    return g_test_failed ? 1 : 0;
}
