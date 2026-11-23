/*
 * test_rfc0094_contracts.c — Validation suite for RFC-0094 / ADR-0087:
 * - Dual-lifecycle contracts (_retain/_release, _init/_deinit/_reset)
 * - Target-driven canvas begin/end polymorphism
 * - OPTICS_GFX_INIT safe initialization macro
 * - Zero-cost inline canvas helpers (<optics/gfx/canvas_helpers.h>)
 * - Font atlas explicit synchronization query
 */

#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <optics/text.h>

static void test_gfx_init_macro(void) {
    /* Lowercase struct form */
    optics_gfx_buffer_desc b1 =
        OPTICS_GFX_INIT(buffer_desc, .size = 1024, .usage = OPTICS_GFX_BUFFER_USAGE_VERTEX,
                        .location = OPTICS_GFX_BUFFER_HOST_VISIBLE);
    OT_EXPECT(b1.type == OPTICS_GFX_TYPE_BUFFER_DESC);
    OT_EXPECT(b1.next == nullptr);
    OT_EXPECT(b1.size == 1024);
    OT_EXPECT(b1.usage == OPTICS_GFX_BUFFER_USAGE_VERTEX);

    /* Uppercase descriptor form */
    optics_gfx_target_desc t1 =
        OPTICS_GFX_INIT(target_desc, .width = 800, .height = 600, .usage = OPTICS_GFX_TARGET_COLOR,
                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    OT_EXPECT(t1.type == OPTICS_GFX_TYPE_TARGET_DESC);
    OT_EXPECT(t1.next == nullptr);
    OT_EXPECT(t1.width == 800);
    OT_EXPECT(t1.height == 600);

    /* CPU target desc */
    optics_gfx_cpu_target_desc ct1 = OPTICS_GFX_INIT(cpu_target_desc, .width = 64, .height = 64,
                                                     .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    OT_EXPECT(ct1.type == OPTICS_GFX_TYPE_CPU_TARGET_DESC);
    OT_EXPECT(ct1.next == nullptr);
    OT_EXPECT(ct1.width == 64);
}

static void test_arena_lifecycle(void) {
    optics_gfx_arena a;
    optics_gfx_result r = optics_gfx_arena_init(&a, 4096, nullptr);
    OT_EXPECT(r == OPTICS_GFX_OK);
    OT_EXPECT(a.capacity == 4096);
    OT_EXPECT(a.used == 0);

    void *p = optics_gfx_arena_alloc(&a, 128);
    OT_EXPECT(p != nullptr);
    OT_EXPECT(a.used >= 128);

    optics_gfx_arena_reset(&a);
    OT_EXPECT(a.used == 0);

    /* Test path create on arena */
    optics_gfx_path *path = nullptr;
    r = optics_gfx_path_create(&path, &a);
    OT_EXPECT(r == OPTICS_GFX_OK);
    OT_EXPECT(path != nullptr);
    optics_gfx_path_move_to(path, 0.0f, 0.0f);
    optics_gfx_path_line_to(path, 10.0f, 10.0f);
    optics_gfx_path_reset(path);

    /* Deinit */
    optics_gfx_arena_deinit(&a);
    OT_EXPECT(a.base == nullptr);
}

static void test_cpu_target_and_canvas_polymorphism(void) {
    /* 1. Create CPU Target */
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(cpu_target_desc, .width = 64, .height = 64,
                                                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *target = nullptr;
    optics_gfx_result r = optics_gfx_target_create_cpu(&t_desc, &target);
    OT_EXPECT(r == OPTICS_GFX_OK);
    OT_EXPECT(target != nullptr);
    OT_EXPECT(optics_gfx_target_width(target) == 64);
    OT_EXPECT(optics_gfx_target_height(target) == 64);

    /* Retain and release */
    optics_gfx_target *retained = optics_gfx_target_retain(target);
    OT_EXPECT(retained == target);
    optics_gfx_target_release(retained);

    /* 2. Create CPU Canvas */
    optics_gfx_canvas_desc c_desc =
        OPTICS_GFX_INIT(canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 64,
                        .height = 64, .scale = 1.0f);
    optics_gfx_canvas *canvas = nullptr;
    r = optics_gfx_canvas_create(&c_desc, &canvas);
    OT_EXPECT(r == OPTICS_GFX_OK);
    OT_EXPECT(canvas != nullptr);

    /* Canvas retain and release check */
    optics_gfx_canvas *c_retained = optics_gfx_canvas_retain(canvas);
    OT_EXPECT(c_retained == canvas);
    optics_gfx_canvas_release(c_retained);

    /* 3. Polymorphic Target-Driven pass bracket */
    optics_gfx_color clear = optics_gfx_color_rgba_premul(10, 20, 30, 255);
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){
                    .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                    .clear_color = &clear,
                    .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET, .target = target}});
    OT_EXPECT(r == OPTICS_GFX_OK);

    /* 4. Use inline canvas helpers from <optics/gfx/canvas_helpers.h> */
    optics_gfx_rect rect = {4.0f, 4.0f, 24.0f, 24.0f};
    optics_gfx_color rect_color = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    optics_gfx_canvas_fill_rect_color(canvas, rect, rect_color);

    optics_gfx_rect rrect = {32.0f, 4.0f, 24.0f, 24.0f};
    optics_gfx_color rrect_color = optics_gfx_color_rgba_premul(0, 255, 0, 255);
    optics_gfx_canvas_fill_rrect(canvas, rrect, 4.0f, rrect_color);

    r = optics_gfx_canvas_end(canvas);
    OT_EXPECT(r == OPTICS_GFX_OK);

    /* 5. Extract pixels */
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_target_cpu_pixels(target, &w, &h, &stride);
    OT_EXPECT(px != nullptr);
    OT_EXPECT(w == 64);
    OT_EXPECT(h == 64);
    OT_EXPECT(stride >= 64 * 4);

    /* 6. Release resources */
    optics_gfx_canvas_release(canvas);
    optics_gfx_target_release(target);
}

static void test_text_lifecycle_and_atlas_sync(void) {
    optics_text_desc desc = {
        .device = nullptr,
        .scale = 1.0f,
    };
    optics_text_ctx *text = nullptr;
    optics_gfx_result r = optics_text_create(&desc, &text);
    if (r == OPTICS_GFX_OK && text) {
        /* Retain / release */
        optics_text_ctx *retained = optics_text_retain(text);
        OT_EXPECT(retained == text);
        optics_text_release(retained);

        /* Query atlas sync */
        bool pending = optics_text_has_pending_uploads(text);
        OT_EXPECT(!pending);

        optics_gfx_result fr = optics_text_flush_atlas(text, &(optics_text_flush_atlas_opts){});
        OT_EXPECT(fr == OPTICS_GFX_OK);

        optics_text_release(text);
    }
}

static void test_adr0088_clean_break(void) {
    /* 1. Geometry Tagged Union size check */
    OT_EXPECT(sizeof(optics_gfx_geometry) <= 48);

    /* 2. Brush and geometry construction */
    optics_gfx_brush solid_b = optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 0, 0, 255));
    OT_EXPECT(solid_b.kind == OPTICS_GFX_BRUSH_SOLID);
    OT_EXPECT(solid_b.opacity == 1.0f);

    optics_gfx_geometry r_geom =
        optics_gfx_geom_rect((optics_gfx_rect){10.0f, 10.0f, 50.0f, 50.0f});
    OT_EXPECT(r_geom.kind == OPTICS_GFX_GEOM_RECT);

    optics_gfx_geometry sq_geom =
        optics_gfx_geom_squircle((optics_gfx_rect){0.0f, 0.0f, 100.0f, 100.0f}, 16.0f, 0.8f);
    OT_EXPECT(sq_geom.kind == OPTICS_GFX_GEOM_SQUIRCLE);
    OT_EXPECT(sq_geom.squircle.curvature == 0.8f);

    /* 3. Encoder and DisplayList pure CPU recording */
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 16384, nullptr) == OPTICS_GFX_OK);

    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    OT_EXPECT(enc != nullptr);

    optics_gfx_encoder_save(enc);
    optics_gfx_encoder_translate(enc, &(optics_gfx_encoder_translate_opts){.x = 10.0f, .y = 10.0f});
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &r_geom, .brush = &solid_b});
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &sq_geom, .brush = &solid_b});
    optics_gfx_encoder_save_layer(
        enc, &(optics_gfx_encoder_save_layer_opts){.bounds = nullptr, .opacity = 0.8f});
    optics_gfx_encoder_restore(enc);
    optics_gfx_encoder_restore(enc);

    optics_gfx_display_list *dl = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &dl) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_display_list_command_count(dl) > 0);
    OT_EXPECT(optics_gfx_display_list_size(dl) > 0);
    OT_EXPECT(dl != nullptr);

    optics_gfx_encoder_destroy(enc);

    /* 4. Play back into CPU canvas via optics_gfx_canvas_submit_display_list */
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(
        cpu_target_desc, .width = 128, .height = 128, .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *target = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &target) == OPTICS_GFX_OK);

    /* Draws only into explicit targets; its own framebuffer is unused. */
    optics_gfx_canvas_desc c_desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 1, .height = 1);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c) == OPTICS_GFX_OK);

    optics_gfx_color clear = 0;
    OT_EXPECT(
        optics_gfx_canvas_begin(c, &(optics_gfx_canvas_pass_desc){
                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                       .clear_color = &clear,
                                       .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                      .target = target}}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = dl}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    optics_gfx_display_list_release(dl);
    optics_gfx_canvas_release(c);
    optics_gfx_target_release(target);
    optics_gfx_arena_deinit(&arena);
}

static void test_adr0090_display_list_immutable_capture(void) {
    optics_gfx_arena caller_arena;
    OT_EXPECT(optics_gfx_arena_init(&caller_arena, 8192, nullptr) == OPTICS_GFX_OK);

    /* Allocate path in caller's transient arena */
    optics_gfx_path *path = nullptr;
    OT_EXPECT(optics_gfx_path_create(&path, &caller_arena) == OPTICS_GFX_OK);
    optics_gfx_path_move_to(path, 10.0f, 10.0f);
    optics_gfx_path_line_to(path, 50.0f, 10.0f);
    optics_gfx_path_line_to(path, 50.0f, 50.0f);
    optics_gfx_path_close(path);

    /* Record into encoder */
    optics_gfx_arena enc_arena;
    OT_EXPECT(optics_gfx_arena_init(&enc_arena, 8192, nullptr) == OPTICS_GFX_OK);
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);

    optics_gfx_geometry p_geom = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = path}};
    optics_gfx_brush solid_b = optics_gfx_brush_solid(optics_gfx_color_rgba_premul(0, 255, 0, 255));
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &p_geom, .brush = &solid_b});

    /* Finish into display list */
    optics_gfx_display_list *dl = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &dl) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_display_list_command_count(dl) == 1);
    OT_EXPECT(optics_gfx_display_list_size(dl) > 0);

    /* Now destroy encoder, reset encoder arena, and completely reset/destroy caller's path and
     * arena */
    optics_gfx_encoder_destroy(enc);
    optics_gfx_arena_deinit(&enc_arena);
    optics_gfx_path_reset(path);
    optics_gfx_arena_deinit(&caller_arena);

    /* Play back display list into CPU canvas — the embedded path segments must survive! */
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(cpu_target_desc, .width = 64, .height = 64,
                                                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *target = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &target) == OPTICS_GFX_OK);

    /* Draws only into explicit targets; its own framebuffer is unused. */
    optics_gfx_canvas_desc c_desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 1, .height = 1);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c) == OPTICS_GFX_OK);

    optics_gfx_color clear = 0;
    OT_EXPECT(
        optics_gfx_canvas_begin(c, &(optics_gfx_canvas_pass_desc){
                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                       .clear_color = &clear,
                                       .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                      .target = target}}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = dl}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Verify pixels at (30, 20) inside the triangle are rendered green */
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_target_cpu_pixels(target, &w, &h, &stride);
    OT_EXPECT(px != nullptr);
    const uint8_t *sample = &px[(20 * w + 30) * 4];
    OT_EXPECT(sample[1] > 200); /* green channel */
    OT_EXPECT(sample[3] > 200); /* alpha channel */

    optics_gfx_display_list_release(dl);
    optics_gfx_canvas_release(c);
    optics_gfx_target_release(target);
}

static void test_adr0091_save_layer_opacity_group(void) {
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(cpu_target_desc, .width = 64, .height = 64,
                                                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *target = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &target) == OPTICS_GFX_OK);

    /* Draws only into explicit targets; its own framebuffer is unused. */
    optics_gfx_canvas_desc c_desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 1, .height = 1);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c) == OPTICS_GFX_OK);

    optics_gfx_color clear = 0;
    OT_EXPECT(
        optics_gfx_canvas_begin(c, &(optics_gfx_canvas_pass_desc){
                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                       .clear_color = &clear,
                                       .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                      .target = target}}) == OPTICS_GFX_OK);

    /* Opacity Group with 0.5 opacity:
     * Draw two overlapping fully-opaque red rectangles.
     * Rect 1: [8, 8, 24, 24]
     * Rect 2: [20, 8, 24, 24]  -> overlap is [20..32, 8..32] */
    optics_gfx_canvas_save_layer(
        c, &(optics_gfx_canvas_save_layer_opts){.bounds = nullptr, .opacity = 0.5f});
    optics_gfx_color red = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){8.0f, 8.0f, 24.0f, 24.0f}, red);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){20.0f, 8.0f, 24.0f, 24.0f}, red);
    optics_gfx_canvas_restore(c);

    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_target_cpu_pixels(target, &w, &h, &stride);
    OT_EXPECT(px != nullptr);

    /* Non-overlapping pixel at (12, 16): Rect 1 only */
    const uint8_t *px_single = &px[(16 * w + 12) * 4];
    /* Overlapping pixel at (24, 16): both Rect 1 and Rect 2 */
    const uint8_t *px_overlap = &px[(16 * w + 24) * 4];

    /* In a true Opacity Group, both pixels MUST have identical alpha and color (~128),
     * NOT the double-blend artifact (191) of immediate opacity */
    OT_EXPECT(abs((int)px_single[3] - 128) <= 4);
    OT_EXPECT(abs((int)px_overlap[3] - 128) <= 4);
    OT_EXPECT(abs((int)px_single[3] - (int)px_overlap[3]) <= 2);
    OT_EXPECT(abs((int)px_single[0] - (int)px_overlap[0]) <= 2);

    optics_gfx_canvas_release(c);
    optics_gfx_target_release(target);
}

static void test_adr0091_squircle_distinct_geometry(void) {
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(cpu_target_desc, .width = 64, .height = 64,
                                                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *t_sq = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &t_sq) == OPTICS_GFX_OK);

    optics_gfx_target *t_rr = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &t_rr) == OPTICS_GFX_OK);

    /* Draws only into explicit targets; its own framebuffer is unused. */
    optics_gfx_canvas_desc c_desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 1, .height = 1);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c) == OPTICS_GFX_OK);

    optics_gfx_color clear = 0;
    optics_gfx_color white = optics_gfx_color_rgba_premul(255, 255, 255, 255);
    optics_gfx_rect box = {4.0f, 4.0f, 56.0f, 56.0f};

    /* Draw squircle */
    OT_EXPECT(
        optics_gfx_canvas_begin(c, &(optics_gfx_canvas_pass_desc){
                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                       .clear_color = &clear,
                                       .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                      .target = t_sq}}) == OPTICS_GFX_OK);
    optics_gfx_geometry g_sq = optics_gfx_geom_squircle(box, 16.0f, 1.0f);
    optics_gfx_brush b = optics_gfx_brush_solid(white);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g_sq, .brush = &b});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* Draw rrect */
    OT_EXPECT(
        optics_gfx_canvas_begin(c, &(optics_gfx_canvas_pass_desc){
                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                       .clear_color = &clear,
                                       .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                      .target = t_rr}}) == OPTICS_GFX_OK);
    optics_gfx_geometry g_rr = optics_gfx_geom_rrect(box, 16.0f);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g_rr, .brush = &b});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px_sq = optics_gfx_target_cpu_pixels(t_sq, &w, &h, &stride);
    const uint8_t *px_rr = optics_gfx_target_cpu_pixels(t_rr, &w, &h, &stride);

    /* Squircle has distinct corner shoulder curvature compared to standard circular arc. */
    bool diff_detected = false;
    for (uint32_t y = 4; y < 20; y++) {
        for (uint32_t x = 4; x < 20; x++) {
            if (px_sq[(y * w + x) * 4 + 3] != px_rr[(y * w + x) * 4 + 3]) {
                diff_detected = true;
                break;
            }
        }
        if (diff_detected)
            break;
    }
    OT_EXPECT(diff_detected);

    optics_gfx_canvas_release(c);
    optics_gfx_target_release(t_sq);
    optics_gfx_target_release(t_rr);
}

static void test_adr0092_text_atlas_epoch_tracking(void) {
    optics_text_desc desc = {.scale = 1.0f};
    optics_text_ctx *text = nullptr;
    OT_EXPECT(optics_text_create(&desc, &text) == OPTICS_GFX_OK);

    uint64_t epoch0 = optics_text_get_atlas_epoch(text, nullptr);
    OT_EXPECT(optics_text_flush_atlas(text, &(optics_text_flush_atlas_opts){}) == OPTICS_GFX_OK);
    uint64_t epoch1 = optics_text_get_atlas_epoch(text, nullptr);
    OT_EXPECT(epoch1 > epoch0);

    optics_text_release(text);
}

static void test_adr0093_target_exclusive_borrow_and_readback(void) {
    optics_gfx_cpu_target_desc t_desc = OPTICS_GFX_INIT(cpu_target_desc, .width = 32, .height = 32,
                                                        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM);
    optics_gfx_target *target = nullptr;
    OT_EXPECT(optics_gfx_target_create_cpu(&t_desc, &target) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc c_desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 32, .height = 32);
    optics_gfx_canvas *c1 = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c1) == OPTICS_GFX_OK);
    optics_gfx_canvas *c2 = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&c_desc, &c2) == OPTICS_GFX_OK);

    /* 1. Begin on target with c1 */
    optics_gfx_color red = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    OT_EXPECT(
        optics_gfx_canvas_begin(c1, &(optics_gfx_canvas_pass_desc){
                                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                        .clear_color = &red,
                                        .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                       .target = target}}) == OPTICS_GFX_OK);

    /* 2. Overlapping begin on the same target with c2 MUST fail with INVALID_STATE (ADR-0093) */
    OT_EXPECT(optics_gfx_canvas_begin(
                  c2, &(optics_gfx_canvas_pass_desc){
                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                          .clear_color = &red,
                          .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                         .target = target}}) == OPTICS_GFX_ERROR_INVALID_STATE);

    /* 3. Readback of target pixels during active session MUST return nullptr */
    uint32_t w = 0, h = 0, stride = 0;
    OT_EXPECT(optics_gfx_target_cpu_pixels(target, &w, &h, &stride) == nullptr);

    /* 4. Canvas readback while recording MUST return nullptr */
    OT_EXPECT(optics_gfx_canvas_read_pixels(c1, &w, &h, &stride) == nullptr);

    /* 5. End session cleanly on c1 */
    OT_EXPECT(optics_gfx_canvas_end(c1) == OPTICS_GFX_OK);

    /* 6. Target readback is now safe and exposed */
    const uint8_t *px = optics_gfx_target_cpu_pixels(target, &w, &h, &stride);
    OT_EXPECT(px != nullptr);
    OT_EXPECT(px[0] == 255); /* Red clear color */

    /* 7. Target can now be bound to c2 */
    optics_gfx_color blue = optics_gfx_color_rgba_premul(0, 0, 255, 255);
    OT_EXPECT(
        optics_gfx_canvas_begin(c2, &(optics_gfx_canvas_pass_desc){
                                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                        .clear_color = &blue,
                                        .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_TARGET,
                                                       .target = target}}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c2) == OPTICS_GFX_OK);
    px = optics_gfx_target_cpu_pixels(target, &w, &h, &stride);
    OT_EXPECT(px != nullptr);
    OT_EXPECT(px[2] == 255); /* Blue clear color */

    optics_gfx_canvas_release(c1);
    optics_gfx_canvas_release(c2);
    optics_gfx_target_release(target);
}

int main(void) {
    test_gfx_init_macro();
    test_arena_lifecycle();
    test_cpu_target_and_canvas_polymorphism();
    test_text_lifecycle_and_atlas_sync();
    test_adr0088_clean_break();
    test_adr0090_display_list_immutable_capture();
    test_adr0091_save_layer_opacity_group();
    test_adr0091_squircle_distinct_geometry();
    test_adr0092_text_atlas_epoch_tracking();
    test_adr0093_target_exclusive_borrow_and_readback();

    return OT_REPORT();
    return OT_FAILURES() ? 1 : 0;
}
