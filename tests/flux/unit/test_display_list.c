#include "test_helpers.h"
#include <flux/canvas.h>
#include <string.h>

static flux_canvas *canvas(void) {
    flux_canvas *c = nullptr;
    EXPECT(flux_canvas_create(&(flux_canvas_desc){
               .type = FLUX_TYPE_CANVAS_DESC,
               .backend = FLUX_CANVAS_BACKEND_CPU,
               .width = 64,
               .height = 64,
           }, &c) == FLUX_OK);
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
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
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
    flux_glyph_quad q = {.sx = 2, .sy = 2, .sw = 8, .sh = 8, .aw = 1, .ah = 1,
                         .color = 0xffffffff};
    flux_encoder_draw_glyph_run(enc, &(flux_glyph_run_desc){
        .type = FLUX_TYPE_GLYPH_RUN_DESC, .host_coverage = coverage,
        .host_atlas_w = 1, .host_atlas_h = 1, .quads = &q, .quad_count = 1,
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
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
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
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
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
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
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
    EXPECT(flux_encoder_append_display_list(enc2, dl1) == FLUX_OK);
    flux_display_list *dl2 = nullptr;
    EXPECT(flux_encoder_finish(enc2, &dl2) == FLUX_OK);
    flux_encoder_destroy(enc2);
    flux_display_list_release(dl1);

    EXPECT(flux_display_list_command_count(dl2) == 2);
    EXPECT(flux_display_list_size(dl2) > 0);

    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, dl2) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 4, 4) == 255);
    EXPECT(alpha(c, 22, 22) == 255);
    EXPECT(alpha(c, 10, 10) == 0);
    flux_canvas_release(c);
    flux_display_list_release(dl2);
}

static void serialization_roundtrip_and_fuzz(void) {
    flux_encoder *enc = nullptr;
    EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
    flux_geometry g = flux_geom_rect((flux_rect){5, 5, 10, 10});
    flux_brush white = flux_brush_solid(0xffffffff);
    flux_encoder_draw_geometry(enc, &g, &white);
    flux_encoder_translate(enc, 2.0f, 3.0f);
    flux_encoder_rotate(enc, 0.5f);
    flux_display_list *dl = nullptr;
    EXPECT(flux_encoder_finish(enc, &dl) == FLUX_OK);
    flux_encoder_destroy(enc);

    /* 1. Serialize */
    void *bytes = nullptr;
    size_t size = 0;
    EXPECT(flux_display_list_serialize(dl, &bytes, &size) == FLUX_OK);
    EXPECT(bytes != nullptr && size > sizeof(uint32_t) * 4);

    /* 2. Deserialize back */
    flux_display_list *deserialized = nullptr;
    EXPECT(flux_display_list_deserialize(bytes, size, &deserialized) == FLUX_OK);
    EXPECT(deserialized != nullptr);
    EXPECT(flux_display_list_command_count(deserialized) == flux_display_list_command_count(dl));
    EXPECT(flux_display_list_size(deserialized) == flux_display_list_size(dl));

    flux_display_list_release(dl);

    /* 3. Replay deserialized list into canvas */
    flux_canvas *c = canvas();
    flux_color clear = 0;
    EXPECT(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
    EXPECT(flux_canvas_submit_display_list(c, deserialized) == FLUX_OK);
    EXPECT(flux_canvas_end(c) == FLUX_OK);
    EXPECT(alpha(c, 8, 8) > 0);
    flux_canvas_release(c);
    flux_display_list_release(deserialized);

    /* 4. Strict fuzzing & corruption resistance (ADR-0090) */
    flux_display_list *bad = nullptr;
    /* Null arguments */
    EXPECT(flux_display_list_deserialize(nullptr, size, &bad) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_display_list_deserialize(bytes, 0, &bad) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_display_list_deserialize(bytes, 4, &bad) == FLUX_ERROR_INVALID_ARGUMENT);

    /* Corrupt magic */
    uint8_t corrupt[256];
    memcpy(corrupt, bytes, size < 256 ? size : 256);
    corrupt[0] ^= 0xFF;
    EXPECT(flux_display_list_deserialize(corrupt, size, &bad) == FLUX_ERROR_UNSUPPORTED);

    /* Corrupt checksum */
    memcpy(corrupt, bytes, size < 256 ? size : 256);
    corrupt[size - 1] ^= 0xFF;
    EXPECT(flux_display_list_deserialize(corrupt, size, &bad) == FLUX_ERROR_BACKEND_FAILURE);

    /* Truncated payload size */
    memcpy(corrupt, bytes, size < 256 ? size : 256);
    corrupt[12] = 0; /* payload_size field */
    EXPECT(flux_display_list_deserialize(corrupt, size, &bad) == FLUX_ERROR_OUT_OF_RANGE);

    free(bytes);
}

int main(void) {
    relocated_capture();
    captured_glyphs();
    isolated_state();
    terminal_and_failure();
    layer_bounds_are_hints();
    append_and_splice();
    serialization_roundtrip_and_fuzz();
    TEST_SUMMARY();
}
