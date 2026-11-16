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
#include "flux_test.h"
#include "optics_test.h"

#include <flux/canvas.h>
#include <flux/canvas_helpers.h>
#include <flux/flux.h>

#include <math.h>
#include <string.h>

#define W 128u
#define H 128u

typedef void (*draw_fn)(flux_canvas *c, flux_arena *arena);

static flux_path *oblique_quad(flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return nullptr;
    flux_path_move_to(p, 0.0f, 6.0f);
    flux_path_line_to(p, 127.0f, 21.0f);
    flux_path_line_to(p, 127.0f, 127.0f);
    flux_path_line_to(p, 0.0f, 127.0f);
    flux_path_close(p);
    return p;
}

static void draw_fill(flux_canvas *c, flux_arena *arena) {
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_path(c, oblique_quad(arena), FLUX_FILL_NON_ZERO, &brush);
}

static void draw_fill_plus(flux_canvas *c, flux_arena *arena) {
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    brush.blend = FLUX_BLEND_PLUS;
    flux_canvas_fill_path(c, oblique_quad(arena), FLUX_FILL_NON_ZERO, &brush);
}

static void draw_stroke(flux_canvas *c, flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    /* Same slope as the fill's edge, centred in the band the probe reads. */
    flux_path_move_to(p, 0.0f, 12.0f);
    flux_path_line_to(p, 127.0f, 27.0f);
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_stroke_path(c, p, &(flux_stroke_style){.width = 6.0f}, &brush);
}

/* A rectangle as a general path (not the rectangle fast path) whose top edge
 * sits a quarter of the way down pixel row 20: that row is exactly 75%
 * covered. A horizontal edge has no slope to hide behind — a rasteriser that
 * derives coverage from anything but area gets this wrong. */
static void draw_fractional_rect(flux_canvas *c, flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    flux_path_move_to(p, 16.0f, 20.25f);
    flux_path_line_to(p, 112.0f, 20.25f);
    flux_path_line_to(p, 112.0f, 100.0f);
    flux_path_line_to(p, 16.0f, 100.0f);
    flux_path_close(p);
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_path(c, p, FLUX_FILL_NON_ZERO, &brush);
}

/* A path whose flattened vertex count exceeds both the legacy 2048-point
 * flattening cap and the 4096-segment GPU bound (~5100 segments), with a
 * shallow slanted edge across rows 10..26 to verify INV-COV-02 over the cap. */
static void draw_long_fill(flux_canvas *c, flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    const uint32_t n_top = 5100;
    flux_path_move_to(p, 0.0f, 10.0f);
    for (uint32_t i = 1; i <= n_top; ++i) {
        float t = (float)i / (float)n_top;
        flux_path_line_to(p, t * 127.0f, 10.0f + t * 16.0f);
    }
    flux_path_line_to(p, 127.0f, 127.0f);
    flux_path_line_to(p, 0.0f, 127.0f);
    flux_path_close(p);
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_path(c, p, FLUX_FILL_NON_ZERO, &brush);
}

/* A stroke whose triangle list exceeds the removed 4096-segment bound, with
 * a shallow diagonal stroke spanning rows 8..24 to assert INV-COV-02. */
static void draw_long_stroke(flux_canvas *c, flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    for (int i = 0; i <= 750; i++) {
        const float t = (float)i / 750.0f;
        const float x = t * 127.0f;
        const float y = 8.0f + t * 16.0f;
        if (i == 0)
            flux_path_move_to(p, x, y);
        else
            flux_path_line_to(p, x, y);
    }
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_stroke_path(c, p, &(flux_stroke_style){.width = 6.0f}, &brush);
}

static void draw_rotated_rect(flux_canvas *c, flux_arena *arena) {
    (void)arena;
    flux_canvas_save(c);
    flux_canvas_translate(c, &(flux_canvas_translate_opts){.x = 64.0f, .y = 64.0f});
    flux_canvas_rotate(c, 0.14f);
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_geometry g = flux_geom_rect((flux_rect){-60.0f, -48.0f, 120.0f, 96.0f});
    flux_canvas_draw_geometry(c, &(flux_canvas_draw_geometry_opts){.geom = &g, .brush = &brush});
    flux_canvas_restore(c);
}

static void draw_rect_fractional_geom(flux_canvas *c, flux_arena *arena) {
    (void)arena;
    flux_canvas_fill_rect_color(c, (flux_rect){16.0f, 20.25f, 96.0f, 80.0f},
                                flux_color_rgba_premul(255, 255, 255, 255));
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

static int measure(flux_surface *s, flux_canvas *gpu, flux_arena *arena, draw_fn draw) {
    flux_color clear = flux_color_rgba(0, 0, 0, 255);
    flux_frame *frame = nullptr;
    OT_REQUIRE(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
    OT_REQUIRE(flux_canvas_begin(gpu, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &clear}) == FLUX_OK);
    draw(gpu, arena);
    OT_EXPECT(flux_canvas_end(gpu) == FLUX_OK);
    OT_EXPECT(flux_frame_submit(frame) == FLUX_OK);
    OT_EXPECT(flux_frame_present(frame) == FLUX_OK);
    OT_EXPECT(flux_surface_read_pixels(s, px, sizeof(px)) == FLUX_OK);
    flux_arena_reset(arena);
    return count_levels(px, W * 4);
}

int main(void) {
    flux_arena arena;
    OT_REQUIRE(flux_arena_init(&arena, 1024 * 1024, nullptr) == FLUX_OK);

    flux_device *d = flux_test_make_headless_device();
    if (!d)
        OT_SKIP("no Vulkan device");

    flux_surface *s = nullptr;
    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    OT_REQUIRE(flux_surface_create(d, &sd, &s) == FLUX_OK);
    flux_canvas *gpu = nullptr;
    flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
    cd.surface = s;
    OT_REQUIRE(flux_canvas_create(&cd, &gpu) == FLUX_OK);

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

    /* Fractional FLUX_GEOM_RECT routes through analytic coverage as well. */
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
        flux_image *img = nullptr;
        OT_REQUIRE(flux_image_create(d,
                                     &(flux_image_desc){
                                         .type = FLUX_TYPE_IMAGE_DESC,
                                         .width = 16,
                                         .height = 16,
                                         .format = FLUX_FORMAT_RGBA8_UNORM,
                                         .usage = FLUX_IMAGE_USAGE_SAMPLED,
                                         .initial_data = white_tex,
                                     },
                                     &img) == FLUX_OK);

        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        flux_frame *frame = nullptr;
        OT_REQUIRE(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        OT_REQUIRE(flux_canvas_begin(gpu, &(flux_canvas_pass_desc){
                                              .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                              .frame = frame,
                                              .clear_color = &clear,
                                          }) == FLUX_OK);
        flux_canvas_save(gpu);
        flux_canvas_translate(gpu, &(flux_canvas_translate_opts){.x = 64.0f, .y = 64.0f});
        flux_canvas_rotate(gpu, 0.14f);
        flux_brush img_brush = {
            .kind = FLUX_BRUSH_IMAGE_PATTERN,
            .blend = FLUX_BLEND_SRC_OVER,
            .opacity = 1.0f,
            .image = {.image = img, .tint = 0xFFFFFFFFu},
        };
        flux_geometry rg = flux_geom_rect((flux_rect){-60.0f, -48.0f, 120.0f, 96.0f});
        flux_canvas_draw_geometry(
            gpu, &(flux_canvas_draw_geometry_opts){.geom = &rg, .brush = &img_brush});
        flux_canvas_restore(gpu);
        OT_EXPECT(flux_canvas_end(gpu) == FLUX_OK);
        OT_EXPECT(flux_frame_submit(frame) == FLUX_OK);
        OT_EXPECT(flux_frame_present(frame) == FLUX_OK);
        OT_EXPECT(flux_surface_read_pixels(s, px, sizeof(px)) == FLUX_OK);
        int rot_img_levels = count_levels(px, W * 4);
        fprintf(stderr, "  %-26s %3d distinct levels\n", "rotated image pattern", rot_img_levels);
        OT_EXPECT(rot_img_levels > 16);

        OT_REQUIRE(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        OT_REQUIRE(flux_canvas_begin(gpu, &(flux_canvas_pass_desc){
                                              .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                              .frame = frame,
                                              .clear_color = &clear,
                                          }) == FLUX_OK);
        flux_geometry pg = flux_geom_path(oblique_quad(&arena));
        flux_canvas_draw_geometry(
            gpu, &(flux_canvas_draw_geometry_opts){.geom = &pg, .brush = &img_brush});
        OT_EXPECT(flux_canvas_end(gpu) == FLUX_OK);
        OT_EXPECT(flux_frame_submit(frame) == FLUX_OK);
        OT_EXPECT(flux_frame_present(frame) == FLUX_OK);
        OT_EXPECT(flux_surface_read_pixels(s, px, sizeof(px)) == FLUX_OK);
        int path_img_levels = count_levels(px, W * 4);
        fprintf(stderr, "  %-26s %3d distinct levels\n", "path image pattern", path_img_levels);
        OT_EXPECT(path_img_levels > 16);

        flux_image_release(img);
    }

    flux_canvas_release(gpu);
    flux_surface_release(s);
    flux_device_release(d);
    flux_arena_deinit(&arena);
    return OT_REPORT();
}
