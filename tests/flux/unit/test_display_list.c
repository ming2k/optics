#include "test_helpers.h"
#include <flux/canvas.h>
#include <string.h>

static flux_canvas *canvas(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(
               &(flux_canvas_desc){
                   .type = FLUX_TYPE_CANVAS_DESC,
                   .backend = FLUX_CANVAS_BACKEND_CPU,
                   .width = 64,
                   .height = 64,
               },
               &c) == FLUX_OK);
    return c;
}

static uint8_t alpha(flux_canvas *c, unsigned x, unsigned y) {
    uint32_t stride = 0;
    const uint8_t *p = flux_canvas_read_pixels(c, nullptr, nullptr, &stride);
    EXPECT(p != nullptr);
    return p ? p[y * stride + x * 4 + 3] : 0;
}

static void relocated_capture(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 4096, nullptr) == FLUX_OK);
    flux_path *path = nullptr;
    EXPECT(flux_path_create(&path, &arena) == FLUX_OK);
    flux_path_add_rect(path, (flux_rect){2, 2, 8, 8});
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = path}};
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_path_reset(path);
    flux_encoder_draw_geometry(enc, &g, &white); /* Empty path owns no caller pointer. */
    flux_arena_deinit(&arena);

    g = flux_geom_rect((flux_rect){0, 0, 0, 0});
    for (unsigned i = 0; i < 10000; i++)
        flux_encoder_draw_geometry(enc, &g, &white);
    flux_encoder_rotate(enc, 0); /* A command requiring alignment padding. */
    g = flux_geom_rect((flux_rect){20, 2, 8, 8});
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_display_list *list = nullptr;
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_OK);
    flux_encoder_destroy(enc);
    EXPECT(flux_display_list_command_count(list) == 10004);
    EXPECT(flux_display_list_size(list) > 4096);
    flux_display_list *shared = flux_display_list_retain(list);
    flux_display_list_release(list);

    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, shared) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 4, 4) == 255);
    EXPECT(alpha(c, 24, 4) == 255);
    EXPECT(alpha(c, 14, 4) == 0);
    flux_canvas_release(c);
    flux_display_list_release(shared);
}

static void captured_glyphs(void) {
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    uint8_t *coverage = malloc(1);
    EXPECT(coverage != nullptr);
    *coverage = 255;
    flux_glyph_quad q = {.sx = 2, .sy = 2, .sw = 8, .sh = 8, .aw = 1, .ah = 1, .color = 0xffffffff};
    flux_encoder_draw_glyph_run(enc, &(flux_glyph_run_desc){
                                         .type = FLUX_TYPE_GLYPH_RUN_DESC,
                                         .host_coverage = coverage,
                                         .host_atlas_w = 1,
                                         .host_atlas_h = 1,
                                         .quads = &q,
                                         .quad_count = 1,
                                     });
    *coverage = 0;
    free(coverage);
    q = (flux_glyph_quad){};
    flux_encoder_rotate(enc, 0);
    flux_display_list *list = nullptr;
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_OK);
    flux_encoder_destroy(enc);
    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, list) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 5, 5) == 255);
    flux_canvas_release(c);
    flux_display_list_release(list);
}

static void isolated_state(void) {
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_geometry g = flux_geom_rect((flux_rect){2, 2, 8, 8});
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_encoder_translate(enc, 10, 0); /* Does not leak into the caller. */
    flux_display_list *list = nullptr;
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_OK);
    flux_encoder_destroy(enc);
    flux_canvas *c = canvas();
    EXPECT(flux_canvas_submit_display_list(c, list) == FLUX_ERROR_INVALID_STATE);
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    flux_canvas_translate(c, 20, 0);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 12, 12});
    EXPECT(flux_canvas_submit_display_list(c, list) == FLUX_OK);
    flux_canvas_draw_geometry(c, &g, &white);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 4, 4) == 255);
    EXPECT(alpha(c, 24, 4) == 255);
    EXPECT(alpha(c, 34, 4) == 0);
    EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_STATE);
    flux_canvas_release(c);
    flux_display_list_release(list);
}

static void terminal_and_failure(void) {
    flux_encoder *enc = nullptr;
    flux_display_list *list = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_OK); /* Empty publication is owned. */
    EXPECT(list != nullptr);
    EXPECT(flux_display_list_size(list) == 0);
    flux_display_list_release(list);
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_ERROR_INVALID_STATE);
    EXPECT(list == nullptr);
    flux_encoder_destroy(enc);

    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_encoder_save(enc);
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_ERROR_INVALID_STATE);
    EXPECT(list == nullptr);
    flux_encoder_destroy(enc);

    EXPECT(flux_encoder_create(&(flux_encoder_desc){.max_bytes = 8}, &enc) == FLUX_OK);
    flux_encoder_save(enc);
    flux_encoder_restore(enc); /* Budget failure; later operations cannot hide it. */
    flux_encoder_rotate(enc, 0);
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_ERROR_OUT_OF_MEMORY);
    EXPECT(list == nullptr);
    flux_encoder_destroy(enc);

    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_encoder_rotate(enc, NAN);
    flux_encoder_rotate(enc, 0);
    EXPECT(flux_encoder_finish(enc, &list) == FLUX_ERROR_INVALID_ARGUMENT);
    flux_encoder_destroy(enc);
}

static void layer_bounds_are_hints(void) {
    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    flux_rect hint = {0, 0, 1, 1};
    flux_canvas_save_layer(c, &hint, 0.5f);
    flux_geometry g = flux_geom_rect((flux_rect){2, 2, 8, 8});
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_canvas_draw_geometry(c, &g, &white);
    flux_canvas_restore(c);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(abs((int)alpha(c, 4, 4) - 128) <= 1);
    flux_canvas_release(c);
}

static void append_and_splice(void) {
    flux_encoder *enc1 = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc1) == FLUX_OK);
    flux_geometry g1 = flux_geom_rect((flux_rect){2, 2, 8, 8});
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_encoder_draw_geometry(enc1, &g1, &white);
    flux_display_list *dl1 = nullptr;
    EXPECT(flux_encoder_finish(enc1, &dl1) == FLUX_OK);
    flux_encoder_destroy(enc1);

    flux_encoder *enc2 = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc2) == FLUX_OK);
    flux_geometry g2 = flux_geom_rect((flux_rect){20, 20, 8, 8});
    flux_encoder_draw_geometry(enc2, &g2, &white);
    EXPECT(flux_encoder_draw_display_list(enc2, dl1) == FLUX_OK);
    flux_display_list *dl2 = nullptr;
    EXPECT(flux_encoder_finish(enc2, &dl2) == FLUX_OK);
    flux_encoder_destroy(enc2);
    flux_display_list_release(dl1);

    EXPECT(flux_display_list_command_count(dl2) == 2);
    EXPECT(flux_display_list_size(dl2) > 0);

    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, dl2) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 4, 4) == 255);
    EXPECT(alpha(c, 22, 22) == 255);
    EXPECT(alpha(c, 10, 10) == 0);
    flux_canvas_release(c);
    flux_display_list_release(dl2);
}

/* A retained child is placed explicitly by recorder state and cannot leak
 * its transforms into its parent. Destroy every builder and child owner
 * before rendering the parent. */
static void child_ownership_and_state(void) {
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_encoder_translate(enc, 2, 0);
    flux_geometry g = flux_geom_rect((flux_rect){0, 0, 4, 4});
    flux_brush white = flux_brush_solid(0xffffffffu);
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_display_list *child = nullptr;
    EXPECT(flux_encoder_finish(enc, &child) == FLUX_OK);
    flux_encoder_destroy(enc);

    /* This budget is smaller than the child's payload; composition must
     * retain a reference rather than copy the captured commands. */
    EXPECT(flux_encoder_create(&(flux_encoder_desc){.max_bytes = 64}, &enc) == FLUX_OK);
    flux_encoder_translate(enc, 8, 8);
    EXPECT(flux_encoder_draw_display_list(enc, child) == FLUX_OK);
    EXPECT(flux_encoder_draw_display_list(enc, child) == FLUX_OK);
    flux_display_list *placed = nullptr;
    EXPECT(flux_encoder_finish(enc, &placed) == FLUX_OK);
    flux_encoder_destroy(enc);
    flux_display_list_release(child);

    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    EXPECT(flux_encoder_draw_display_list(enc, placed) == FLUX_OK);
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_display_list *parent = nullptr;
    EXPECT(flux_encoder_finish(enc, &parent) == FLUX_OK);
    flux_encoder_destroy(enc);
    flux_display_list_release(placed);

    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, parent) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 1, 1) == 255);
    EXPECT(alpha(c, 11, 9) == 255);
    EXPECT(alpha(c, 15, 9) == 0);
    flux_canvas_release(c);
    flux_display_list_release(parent);
}

static void child_nesting_and_sticky_failure(void) {
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_geometry g = flux_geom_rect((flux_rect){0, 0, 4, 4});
    flux_brush b = flux_brush_solid(0xffffffffu);
    flux_encoder_draw_geometry(enc, &g, &b);
    flux_display_list *child = nullptr;
    EXPECT(flux_encoder_finish(enc, &child) == FLUX_OK);
    flux_encoder_destroy(enc);
    bool rejected = false;
    for (unsigned i = 0; i < 512; i++) {
        EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
        flux_result result = flux_encoder_draw_display_list(enc, child);
        flux_display_list *parent = nullptr;
        if (result != FLUX_OK) {
            EXPECT(result == FLUX_ERROR_OUT_OF_RANGE);
            flux_encoder_draw_geometry(enc, &g, &b);
            EXPECT(flux_encoder_finish(enc, &parent) == result);
            EXPECT(parent == nullptr);
            flux_encoder_destroy(enc);
            rejected = true;
            break;
        }
        EXPECT(flux_encoder_finish(enc, &parent) == FLUX_OK);
        flux_encoder_destroy(enc);
        flux_display_list_release(child);
        child = parent;
    }
    EXPECT(rejected);
    flux_display_list_release(child);
}

static void cropped_glyph_capture_and_execution_budget(void) {
    uint8_t *coverage = calloc(2048u * 2048u, 1);
    EXPECT(coverage != nullptr);
    coverage[1000u * 2048u + 1000u] = 255;
    flux_glyph_quad q = {.sx = 2,
                         .sy = 2,
                         .sw = 8,
                         .sh = 8,
                         .ax = 1000,
                         .ay = 1000,
                         .aw = 1,
                         .ah = 1,
                         .color = 0xffffffffu};
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(&(flux_encoder_desc){.max_bytes = 512}, &enc) == FLUX_OK);
    flux_encoder_draw_glyph_run(enc, &(flux_glyph_run_desc){.type = FLUX_TYPE_GLYPH_RUN_DESC,
                                                            .host_coverage = coverage,
                                                            .host_atlas_w = 2048,
                                                            .host_atlas_h = 2048,
                                                            .quads = &q,
                                                            .quad_count = 1});
    free(coverage);
    flux_display_list *child = nullptr;
    EXPECT(flux_encoder_finish(enc, &child) == FLUX_OK);
    EXPECT(flux_display_list_size(child) <= 512);
    flux_encoder_destroy(enc);
    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, child) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 4, 4) == 255);
    flux_canvas_release(c);

    EXPECT(flux_encoder_create(&(flux_encoder_desc){.max_commands = 2}, &enc) == FLUX_OK);
    EXPECT(flux_encoder_draw_display_list(enc, child) == FLUX_OK);
    EXPECT(flux_encoder_draw_display_list(enc, child) == FLUX_OK);
    EXPECT(flux_encoder_draw_display_list(enc, child) == FLUX_ERROR_OUT_OF_RANGE);
    flux_display_list *failed = nullptr;
    EXPECT(flux_encoder_finish(enc, &failed) == FLUX_ERROR_OUT_OF_RANGE);
    EXPECT(failed == nullptr);
    flux_encoder_destroy(enc);
    flux_display_list_release(child);
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
    TEST_SUMMARY();
}
