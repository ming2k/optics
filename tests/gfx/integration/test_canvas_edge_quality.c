/*
 * Edge quality on the device, per draw class.
 *
 * The coverage contract (ADR-0116, INV-COV-02) is "more than 16 distinct
 * coverage levels along an oblique edge" — more than any 4x sample count can
 * express. test_canvas_coverage_ceiling pins it for a solid source-over fill.
 * That is one draw class out of several, and the others reach the rasteriser
 * by different routes. A route that falls back to bare triangles in a
 * one-sample pass has exactly two levels: it is aliased.
 *
 * This test draws the same oblique geometry through every class the canvas
 * accepts and holds each to the same contract, so a class cannot silently
 * lose its antialiasing when the default pass policy changes.
 */
#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_helpers.h>

#include <math.h>
#include <string.h>

#define W 128u
#define H 128u

typedef void (*draw_fn)(optics_gfx_canvas *c, optics_gfx_arena *arena);

static optics_gfx_path *oblique_quad(optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return nullptr;
    optics_gfx_path_move_to(p, 0.0f, 6.0f);
    optics_gfx_path_line_to(p, 127.0f, 21.0f);
    optics_gfx_path_line_to(p, 127.0f, 127.0f);
    optics_gfx_path_line_to(p, 0.0f, 127.0f);
    optics_gfx_path_close(p);
    return p;
}

static void draw_fill(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_path(c, oblique_quad(arena), OPTICS_GFX_FILL_NON_ZERO, &brush);
}

static void draw_fill_plus(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    brush.blend = OPTICS_GFX_BLEND_PLUS;
    optics_gfx_canvas_fill_path(c, oblique_quad(arena), OPTICS_GFX_FILL_NON_ZERO, &brush);
}

static void draw_stroke(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    /* Same slope as the fill's edge, centred in the band the probe reads. */
    optics_gfx_path_move_to(p, 0.0f, 12.0f);
    optics_gfx_path_line_to(p, 127.0f, 27.0f);
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_stroke_path(c, p, &(optics_gfx_stroke_style){.width = 6.0f}, &brush);
}

/* A rectangle as a general path (not the rectangle fast path) whose top edge
 * sits a quarter of the way down pixel row 20: that row is exactly 75%
 * covered. A horizontal edge has no slope to hide behind — a rasteriser that
 * derives coverage from anything but area gets this wrong. */
static void draw_fractional_rect(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    optics_gfx_path_move_to(p, 16.0f, 20.25f);
    optics_gfx_path_line_to(p, 112.0f, 20.25f);
    optics_gfx_path_line_to(p, 112.0f, 100.0f);
    optics_gfx_path_line_to(p, 16.0f, 100.0f);
    optics_gfx_path_close(p);
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_path(c, p, OPTICS_GFX_FILL_NON_ZERO, &brush);
}

/* A path whose flattened vertex count exceeds both the legacy 2048-point
 * flattening cap and the 4096-segment GPU bound (~5100 segments), with a
 * shallow slanted edge across rows 10..26 to verify INV-COV-02 over the cap. */
static void draw_long_fill(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    const uint32_t n_top = 5100;
    optics_gfx_path_move_to(p, 0.0f, 10.0f);
    for (uint32_t i = 1; i <= n_top; ++i) {
        float t = (float)i / (float)n_top;
        optics_gfx_path_line_to(p, t * 127.0f, 10.0f + t * 16.0f);
    }
    optics_gfx_path_line_to(p, 127.0f, 127.0f);
    optics_gfx_path_line_to(p, 0.0f, 127.0f);
    optics_gfx_path_close(p);
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_path(c, p, OPTICS_GFX_FILL_NON_ZERO, &brush);
}

/* A stroke whose triangle list exceeds the removed 4096-segment bound, with
 * a shallow diagonal stroke spanning rows 8..24 to assert INV-COV-02. */
static void draw_long_stroke(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    for (int i = 0; i <= 750; i++) {
        const float t = (float)i / 750.0f;
        const float x = t * 127.0f;
        const float y = 8.0f + t * 16.0f;
        if (i == 0)
            optics_gfx_path_move_to(p, x, y);
        else
            optics_gfx_path_line_to(p, x, y);
    }
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_stroke_path(c, p, &(optics_gfx_stroke_style){.width = 6.0f}, &brush);
}

static void draw_rotated_rect(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    (void)arena;
    optics_gfx_canvas_save(c);
    optics_gfx_canvas_translate(c, &(optics_gfx_canvas_translate_opts){.x = 64.0f, .y = 64.0f});
    optics_gfx_canvas_rotate(c, 0.14f);
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){-60.0f, -48.0f, 120.0f, 96.0f});
    optics_gfx_canvas_draw_geometry(
        c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &brush});
    optics_gfx_canvas_restore(c);
}

static void draw_rect_fractional_geom(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    (void)arena;
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){16.0f, 20.25f, 96.0f, 80.0f},
                                      optics_gfx_color_rgba_premul(255, 255, 255, 255));
}

/* Distinct green values in the band that contains the slanted edges. */
static int count_levels(const uint8_t *px, uint32_t stride) {
    bool seen[256] = {};
    for (uint32_t y = 2; y < 40; ++y)
        for (uint32_t x = 0; x < W; ++x)
            seen[px[(size_t)y * stride + x * 4 + 1]] = true;
    int n = 0;
    for (int i = 0; i < 256; ++i)
        n += seen[i];
    return n;
}

static uint8_t px[W * H * 4];

static const uint8_t *px_at(uint32_t x, uint32_t y) {
    return &px[((size_t)y * W + x) * 4];
}

static int measure(optics_gfx_surface *s, optics_gfx_canvas *gpu, optics_gfx_arena *arena,
                   draw_fn draw) {
    optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
    optics_gfx_frame *frame = nullptr;
    OT_REQUIRE(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   gpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                       .frame = frame,
                                                       .clear_color = &clear}) == OPTICS_GFX_OK);
    draw(gpu, arena);
    OT_EXPECT(optics_gfx_canvas_end(gpu) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_surface_read_pixels(s, px, sizeof(px)) == OPTICS_GFX_OK);
    optics_gfx_arena_reset(arena);
    return count_levels(px, W * 4);
}

int main(void) {
    optics_gfx_arena arena;
    OT_REQUIRE(optics_gfx_arena_init(&arena, 1024 * 1024, nullptr) == OPTICS_GFX_OK);

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d)
        OT_SKIP("no Vulkan device");

    optics_gfx_surface *s = nullptr;
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    OT_REQUIRE(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    optics_gfx_canvas *gpu = nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
    cd.surface = s;
    OT_REQUIRE(optics_gfx_canvas_create(&cd, &gpu) == OPTICS_GFX_OK);

    static const struct {
        const char *name;
        draw_fn draw;
    } classes[] = {
        {"solid fill, source-over", draw_fill},
        {"solid fill, plus", draw_fill_plus},
        {"stroke", draw_stroke},
        {"long fill (>5k segs)", draw_long_fill},
        {"long stroke (>4k edges)", draw_long_stroke},
        {"rotated solid rect", draw_rotated_rect},
    };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        const int levels = measure(s, gpu, &arena, classes[i].draw);
        fprintf(stderr, "  %-26s %3d distinct levels\n", classes[i].name, levels);
        /* INV-COV-02, for every class. */
        OT_EXPECT(levels > 16);
    }

    /* Exact area, not an edge ramp: the 75%-covered row reads 75% (in the
     * canvas's linear working space, encoded to sRGB on output), the rows
     * either side are empty and full, and every pixel along the edge agrees. */
    (void)measure(s, gpu, &arena, draw_fractional_rect);
    {
        const double expect = 255.0 * (1.055 * pow(0.75, 1.0 / 2.4) - 0.055);
        OT_EXPECT(px_at(64, 19)[1] <= 2);
        OT_EXPECT(px_at(64, 21)[1] >= 253);
        OT_EXPECT_NEAR((double)px_at(64, 20)[1], expect, 3.0);
        /* The output dither costs up to one LSB (ADR-0069). */
        OT_EXPECT_NEAR((double)px_at(20, 20)[1], (double)px_at(64, 20)[1], 2.0);
        OT_EXPECT_NEAR((double)px_at(108, 20)[1], (double)px_at(64, 20)[1], 2.0);
    }

    /* Fractional OPTICS_GFX_GEOM_RECT routes through analytic coverage as well. */
    (void)measure(s, gpu, &arena, draw_rect_fractional_geom);
    {
        const double expect = 255.0 * (1.055 * pow(0.75, 1.0 / 2.4) - 0.055);
        OT_EXPECT(px_at(64, 19)[1] <= 2);
        OT_EXPECT(px_at(64, 21)[1] >= 253);
        OT_EXPECT_NEAR((double)px_at(64, 20)[1], expect, 3.0);
    }

    /* Image pattern: rotated rect and oblique path both produce continuous coverage. */
    {
        static uint8_t white_tex[16 * 16 * 4];
        memset(white_tex, 255, sizeof(white_tex));
        optics_gfx_image *img = nullptr;
        OT_REQUIRE(optics_gfx_image_create(d,
                                           &(optics_gfx_image_desc){
                                               .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = 16,
                                               .height = 16,
                                               .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_SAMPLED,
                                               .initial_data = white_tex,
                                           },
                                           &img) == OPTICS_GFX_OK);

        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        optics_gfx_frame *frame = nullptr;
        OT_REQUIRE(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_REQUIRE(optics_gfx_canvas_begin(gpu, &(optics_gfx_canvas_pass_desc){
                                                    .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .frame = frame,
                                                    .clear_color = &clear,
                                                }) == OPTICS_GFX_OK);
        optics_gfx_canvas_save(gpu);
        optics_gfx_canvas_translate(gpu,
                                    &(optics_gfx_canvas_translate_opts){.x = 64.0f, .y = 64.0f});
        optics_gfx_canvas_rotate(gpu, 0.14f);
        optics_gfx_brush img_brush = {
            .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
            .blend = OPTICS_GFX_BLEND_SRC_OVER,
            .opacity = 1.0f,
            .image = {.image = img, .tint = 0xFFFFFFFFu},
        };
        optics_gfx_geometry rg =
            optics_gfx_geom_rect((optics_gfx_rect){-60.0f, -48.0f, 120.0f, 96.0f});
        optics_gfx_canvas_draw_geometry(
            gpu, &(optics_gfx_canvas_draw_geometry_opts){.geom = &rg, .brush = &img_brush});
        optics_gfx_canvas_restore(gpu);
        OT_EXPECT(optics_gfx_canvas_end(gpu) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, sizeof(px)) == OPTICS_GFX_OK);
        int rot_img_levels = count_levels(px, W * 4);
        fprintf(stderr, "  %-26s %3d distinct levels\n", "rotated image pattern", rot_img_levels);
        OT_EXPECT(rot_img_levels > 16);

        OT_REQUIRE(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_REQUIRE(optics_gfx_canvas_begin(gpu, &(optics_gfx_canvas_pass_desc){
                                                    .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .frame = frame,
                                                    .clear_color = &clear,
                                                }) == OPTICS_GFX_OK);
        optics_gfx_geometry pg = optics_gfx_geom_path(oblique_quad(&arena));
        optics_gfx_canvas_draw_geometry(
            gpu, &(optics_gfx_canvas_draw_geometry_opts){.geom = &pg, .brush = &img_brush});
        OT_EXPECT(optics_gfx_canvas_end(gpu) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, sizeof(px)) == OPTICS_GFX_OK);
        int path_img_levels = count_levels(px, W * 4);
        fprintf(stderr, "  %-26s %3d distinct levels\n", "path image pattern", path_img_levels);
        OT_EXPECT(path_img_levels > 16);

        optics_gfx_image_release(img);
    }

    optics_gfx_canvas_release(gpu);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
