#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <string.h>

static optics_gfx_canvas *canvas(void) {
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(
                  &(optics_gfx_canvas_desc){
                      .type = OPTICS_GFX_TYPE_CANVAS_DESC,
                      .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                      .width = 64,
                      .height = 64,
                  },
                  &c) == OPTICS_GFX_OK);
    return c;
}

static uint8_t alpha(optics_gfx_canvas *c, unsigned x, unsigned y) {
    uint32_t stride = 0;
    const uint8_t *p = optics_gfx_canvas_read_pixels(c, nullptr, nullptr, &stride);
    OT_EXPECT(p != nullptr);
    return p ? p[y * stride + x * 4 + 3] : 0;
}

static void relocated_capture(void) {
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 4096, nullptr) == OPTICS_GFX_OK);
    optics_gfx_path *path = nullptr;
    OT_EXPECT(optics_gfx_path_create(&path, &arena) == OPTICS_GFX_OK);
    optics_gfx_path_add_rect(path, (optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_geometry g = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = path}};
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_path_reset(path);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){
                 .geom = &g, .brush = &white}); /* Empty path owns no caller pointer. */
    optics_gfx_arena_deinit(&arena);

    g = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 0, 0});
    for (unsigned i = 0; i < 10000; i++)
        optics_gfx_encoder_draw_geometry(
            enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_encoder_rotate(enc, 0); /* A command requiring alignment padding. */
    g = optics_gfx_geom_rect((optics_gfx_rect){20, 2, 8, 8});
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    OT_EXPECT(optics_gfx_display_list_command_count(list) == 10004);
    OT_EXPECT(optics_gfx_display_list_size(list) > 4096);
    optics_gfx_display_list *shared = optics_gfx_display_list_retain(list);
    optics_gfx_display_list_release(list);

    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = shared}) ==
              OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 4, 4) == 255);
    OT_EXPECT(alpha(c, 24, 4) == 255);
    OT_EXPECT(alpha(c, 14, 4) == 0);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(shared);
}

static void captured_glyphs(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    uint8_t *coverage = malloc(1);
    OT_EXPECT(coverage != nullptr);
    *coverage = 255;
    optics_gfx_glyph_quad q = {
        .sx = 2, .sy = 2, .sw = 8, .sh = 8, .aw = 1, .ah = 1, .color = 0xffffffff};
    optics_gfx_encoder_draw_glyph_run(enc, &(optics_gfx_glyph_run_desc){
                                               .type = OPTICS_GFX_TYPE_GLYPH_RUN_DESC,
                                               .host_coverage = coverage,
                                               .host_atlas_w = 1,
                                               .host_atlas_h = 1,
                                               .quads = &q,
                                               .quad_count = 1,
                                           });
    *coverage = 0;
    free(coverage);
    q = (optics_gfx_glyph_quad){};
    optics_gfx_encoder_rotate(enc, 0);
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 5, 5) == 255);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(list);
}

static void isolated_state(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_encoder_translate(enc, &(optics_gfx_encoder_translate_opts){
                                          .x = 10, .y = 0}); /* Does not leak into the caller. */
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_canvas *c = canvas();
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) ==
              OPTICS_GFX_ERROR_INVALID_STATE);
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_translate(c, &(optics_gfx_canvas_translate_opts){.x = 20, .y = 0});
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 12, 12});
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &white});
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 4, 4) == 255);
    OT_EXPECT(alpha(c, 24, 4) == 255);
    OT_EXPECT(alpha(c, 34, 4) == 0);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_STATE);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(list);
}

static void terminal_and_failure(void) {
    optics_gfx_encoder *enc = nullptr;
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) ==
              OPTICS_GFX_OK); /* Empty publication is owned. */
    OT_EXPECT(list != nullptr);
    OT_EXPECT(optics_gfx_display_list_size(list) == 0);
    optics_gfx_display_list_release(list);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(list == nullptr);
    optics_gfx_encoder_destroy(enc);

    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_save(enc);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(list == nullptr);
    optics_gfx_encoder_destroy(enc);

    OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){.max_bytes = 8}, &enc) ==
              OPTICS_GFX_OK);
    optics_gfx_encoder_save(enc);
    optics_gfx_encoder_restore(enc); /* Budget failure; later operations cannot hide it. */
    optics_gfx_encoder_rotate(enc, 0);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
    OT_EXPECT(list == nullptr);
    optics_gfx_encoder_destroy(enc);

    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_rotate(enc, NAN);
    optics_gfx_encoder_rotate(enc, 0);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    optics_gfx_encoder_destroy(enc);
}

static void layer_bounds_are_hints(void) {
    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_rect hint = {0, 0, 1, 1};
    optics_gfx_canvas_save_layer(
        c, &(optics_gfx_canvas_save_layer_opts){.bounds = &hint, .opacity = 0.5f});
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_canvas_restore(c);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(abs((int)alpha(c, 4, 4) - 128) <= 1);
    optics_gfx_canvas_release(c);
}

static void append_and_splice(void) {
    optics_gfx_encoder *enc1 = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc1) == OPTICS_GFX_OK);
    optics_gfx_geometry g1 = optics_gfx_geom_rect((optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_encoder_draw_geometry(
        enc1, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g1, .brush = &white});
    optics_gfx_display_list *dl1 = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc1, &dl1) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc1);

    optics_gfx_encoder *enc2 = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc2) == OPTICS_GFX_OK);
    optics_gfx_geometry g2 = optics_gfx_geom_rect((optics_gfx_rect){20, 20, 8, 8});
    optics_gfx_encoder_draw_geometry(
        enc2, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g2, .brush = &white});
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc2, dl1) == OPTICS_GFX_OK);
    optics_gfx_display_list *dl2 = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc2, &dl2) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc2);
    optics_gfx_display_list_release(dl1);

    OT_EXPECT(optics_gfx_display_list_command_count(dl2) == 2);
    OT_EXPECT(optics_gfx_display_list_size(dl2) > 0);

    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = dl2}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 4, 4) == 255);
    OT_EXPECT(alpha(c, 22, 22) == 255);
    OT_EXPECT(alpha(c, 10, 10) == 0);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(dl2);
}

/* A retained child is placed explicitly by recorder state and cannot leak
 * its transforms into its parent. Destroy every builder and child owner
 * before rendering the parent. */
static void child_ownership_and_state(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_translate(enc, &(optics_gfx_encoder_translate_opts){.x = 2, .y = 0});
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 4, 4});
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffffu);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_display_list *child = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &child) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);

    /* This budget is smaller than the child's payload; composition must
     * retain a reference rather than copy the captured commands. */
    OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){.max_bytes = 64}, &enc) ==
              OPTICS_GFX_OK);
    optics_gfx_encoder_translate(enc, &(optics_gfx_encoder_translate_opts){.x = 8, .y = 8});
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, child) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, child) == OPTICS_GFX_OK);
    optics_gfx_display_list *placed = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &placed) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_display_list_release(child);

    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, placed) == OPTICS_GFX_OK);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_display_list *parent = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &parent) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_display_list_release(placed);

    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = parent}) ==
              OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 1, 1) == 255);
    OT_EXPECT(alpha(c, 11, 9) == 255);
    OT_EXPECT(alpha(c, 15, 9) == 0);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(parent);
}

static void child_nesting_and_sticky_failure(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 4, 4});
    optics_gfx_brush b = optics_gfx_brush_solid(0xffffffffu);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &b});
    optics_gfx_display_list *child = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &child) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    bool rejected = false;
    for (unsigned i = 0; i < 512; i++) {
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
        optics_gfx_result result = optics_gfx_encoder_draw_display_list(enc, child);
        optics_gfx_display_list *parent = nullptr;
        if (result != OPTICS_GFX_OK) {
            OT_EXPECT(result == OPTICS_GFX_ERROR_OUT_OF_RANGE);
            optics_gfx_encoder_draw_geometry(
                enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &b});
            OT_EXPECT(optics_gfx_encoder_finish(enc, &parent) == result);
            OT_EXPECT(parent == nullptr);
            optics_gfx_encoder_destroy(enc);
            rejected = true;
            break;
        }
        OT_EXPECT(optics_gfx_encoder_finish(enc, &parent) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(enc);
        optics_gfx_display_list_release(child);
        child = parent;
    }
    OT_EXPECT(rejected);
    optics_gfx_display_list_release(child);
}

static void cropped_glyph_capture_and_execution_budget(void) {
    uint8_t *coverage = calloc(2048u * 2048u, 1);
    OT_EXPECT(coverage != nullptr);
    coverage[1000u * 2048u + 1000u] = 255;
    optics_gfx_glyph_quad q = {.sx = 2,
                               .sy = 2,
                               .sw = 8,
                               .sh = 8,
                               .ax = 1000,
                               .ay = 1000,
                               .aw = 1,
                               .ah = 1,
                               .color = 0xffffffffu};
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){.max_bytes = 512}, &enc) ==
              OPTICS_GFX_OK);
    optics_gfx_encoder_draw_glyph_run(
        enc, &(optics_gfx_glyph_run_desc){.type = OPTICS_GFX_TYPE_GLYPH_RUN_DESC,
                                          .host_coverage = coverage,
                                          .host_atlas_w = 2048,
                                          .host_atlas_h = 2048,
                                          .quads = &q,
                                          .quad_count = 1});
    free(coverage);
    optics_gfx_display_list *child = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &child) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_display_list_size(child) <= 512);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = child}) ==
              OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 4, 4) == 255);
    optics_gfx_canvas_release(c);

    OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){.max_commands = 2}, &enc) ==
              OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, child) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, child) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_encoder_draw_display_list(enc, child) == OPTICS_GFX_ERROR_OUT_OF_RANGE);
    optics_gfx_display_list *failed = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &failed) == OPTICS_GFX_ERROR_OUT_OF_RANGE);
    OT_EXPECT(failed == nullptr);
    optics_gfx_encoder_destroy(enc);
    optics_gfx_display_list_release(child);
}

static void dynamic_image_inputs(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_brush slot_brush = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.tint = 0xffffffffu},
    };
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){
                 .geom = &g, .brush = &slot_brush, .slot = 3, .use_slot = true});
    OT_EXPECT(optics_gfx_encoder_status(enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g,
                                                      .brush = &slot_brush,
                                                      .slot = OPTICS_GFX_DISPLAY_LIST_MAX_INPUTS,
                                                      .use_slot = true});
    OT_EXPECT(optics_gfx_encoder_status(enc) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    /* The sticky failure keeps the invalid slot out of the published list. */
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(list == nullptr);
    optics_gfx_encoder_destroy(enc);

    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){
                 .geom = &g, .brush = &slot_brush, .slot = 3, .use_slot = true});
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);
    OT_EXPECT(optics_gfx_display_list_input_mask(list) == (UINT64_C(1) << 3));

    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list}) ==
              OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list,
                                                                   .bindings = nullptr,
                                                                   .binding_count = 0}) ==
              OPTICS_GFX_ERROR_INVALID_STATE);
    optics_gfx_display_list_binding missing = {.slot = 3, .image = nullptr};
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list,
                                                                   .bindings = &missing,
                                                                   .binding_count = 1}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    optics_gfx_display_list_binding unknown = {.slot = 4, .image = (optics_gfx_image *)1};
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = list,
                                                                   .bindings = &unknown,
                                                                   .binding_count = 1}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    optics_gfx_canvas_release(c);
    optics_gfx_display_list_release(list);
}

static void serialized_round_trip(void) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){2, 2, 8, 8});
    optics_gfx_brush white = optics_gfx_brush_solid(0xffffffff);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_encoder_translate(enc, &(optics_gfx_encoder_translate_opts){.x = 20, .y = 0});
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &white});
    optics_gfx_display_list *list = nullptr;
    OT_EXPECT(optics_gfx_encoder_finish(enc, &list) == OPTICS_GFX_OK);
    optics_gfx_encoder_destroy(enc);

    uint8_t *data = nullptr;
    size_t size = 0;
    optics_gfx_display_list_serialize_opts sopts = OPTICS_GFX_DISPLAY_LIST_SERIALIZE_OPTS_INIT;
    sopts.out_data = &data;
    sopts.out_size = &size;
    OT_EXPECT(optics_gfx_display_list_serialize(list, &sopts) == OPTICS_GFX_OK);
    OT_EXPECT(data != nullptr && size > 24);

    optics_gfx_display_list *loaded = nullptr;
    optics_gfx_display_list_deserialize_opts dopts = OPTICS_GFX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
    dopts.data = data;
    dopts.size = size;
    dopts.out_list = &loaded;
    OT_EXPECT(optics_gfx_display_list_deserialize(&dopts) == OPTICS_GFX_OK);
    OT_EXPECT(loaded != nullptr);
    optics_gfx_canvas *c = canvas();
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  c, &(optics_gfx_canvas_submit_display_list_opts){.list = loaded}) ==
              OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(alpha(c, 4, 4) == 255);
    OT_EXPECT(alpha(c, 24, 4) == 255);
    OT_EXPECT(alpha(c, 44, 4) == 0);
    optics_gfx_canvas_release(c);

    /* Malformed streams fail closed and publish nothing. */
    optics_gfx_display_list *bad = nullptr;
    uint8_t saved = data[0];
    data[0] ^= 0xFFu;
    dopts.out_list = &bad;
    OT_EXPECT(optics_gfx_display_list_deserialize(&dopts) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(bad == nullptr);
    data[0] = saved;
    dopts.size = size - 1;
    OT_EXPECT(optics_gfx_display_list_deserialize(&dopts) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(bad == nullptr);

    optics_gfx_display_list_serialized_free(data);
    optics_gfx_display_list_release(loaded);
    optics_gfx_display_list_release(list);
}

int main(void) {
    relocated_capture();
    captured_glyphs();
    isolated_state();
    terminal_and_failure();
    layer_bounds_are_hints();
    append_and_splice();
    child_ownership_and_state();
    cropped_glyph_capture_and_execution_budget();
    child_nesting_and_sticky_failure();
    dynamic_image_inputs();
    serialized_round_trip();
    return OT_REPORT();
}
