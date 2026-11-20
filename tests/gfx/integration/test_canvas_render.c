/*
 * Canvas GPU rendering: gradients, strokes, and hole-bridged fills
 * drawn into an offscreen surface (ADR-0013) and asserted by pixel.
 *
 * The unit suite already proves the CPU geometry (flattener,
 * stroker, tessellator) against stubbed submission; this test runs
 * the same features through the real pipelines — paint-kind
 * selection (ADR-0004), push-constant gradient stops, vertex
 * pulling — and reads the bytes back. The donut case is the first
 * GPU run of the hole-bridging merge (ADR-0011).
 */
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>
#if defined(OPTICS_GFX_TEXT_HAVE_FTHB)
#include <optics/text.h>
#endif
#include "gfx_test.h"
#include "optics_test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u
#define BYTES (W * H * 4u)

typedef struct snapshot_stress {
    optics_gfx_image *image;
    atomic_bool stop;
    atomic_uint failures;
} snapshot_stress;

static void *snapshot_worker(void *user) {
    snapshot_stress *stress = user;
    while (!atomic_load_explicit(&stress->stop, memory_order_relaxed)) {
        optics_gfx_image *snapshot = nullptr;
        if (optics_gfx_image_snapshot(stress->image, &snapshot) == OPTICS_GFX_OK)
            optics_gfx_image_release(snapshot);
        else
            atomic_fetch_add_explicit(&stress->failures, 1u, memory_order_relaxed);
    }
    return nullptr;
}

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

typedef void (*draw_fn)(optics_gfx_canvas *canvas, void *user);

typedef struct display_list_case {
    const optics_gfx_display_list *list;
} display_list_case;

static uint32_t test_resource_id(void *user, optics_gfx_display_list_resource_kind kind,
                                 const void *resource) {
    return kind == OPTICS_GFX_DISPLAY_LIST_RESOURCE_IMAGE && resource == user ? 7u : 0u;
}

static const void *test_resource_resolve(void *user, optics_gfx_display_list_resource_kind kind,
                                         uint32_t id) {
    return kind == OPTICS_GFX_DISPLAY_LIST_RESOURCE_IMAGE && id == 7u ? user : nullptr;
}

static void draw_display_list(optics_gfx_canvas *canvas, void *user) {
    const display_list_case *c = user;
    OT_EXPECT(optics_gfx_canvas_submit_display_list(
                  canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = c->list}) ==
              OPTICS_GFX_OK);
}

static optics_gfx_result render_frame(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                      draw_fn draw, void *user) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &clear});
    if (r != OPTICS_GFX_OK)
        return r;
    draw(canvas, user);
    r = optics_gfx_canvas_end(canvas);
    if (r != OPTICS_GFX_OK)
        return r;

    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

static optics_gfx_result render_frame_unknown_ext(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                                  draw_fn draw, void *user) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
    optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pass.clear_color = &clear;
    /* Unknown pNext entries are skipped for forward compatibility. */
    const struct {
        optics_gfx_struct_type type;
        const void *next;
    } future_extension = {OPTICS_GFX_TYPE_UNKNOWN, nullptr};
    pass.next = &future_extension;
    pass.frame = frame;
    r = optics_gfx_canvas_begin(canvas, &pass);
    if (r != OPTICS_GFX_OK)
        return r;
    draw(canvas, user);
    optics_gfx_result pass_result = optics_gfx_canvas_end(canvas);

    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_frame_present(frame);
    return pass_result != OPTICS_GFX_OK ? pass_result : r;
}

/* --- draw callbacks ------------------------------------------------ */

static void draw_linear_gradient(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    optics_gfx_gradient_stop stops[2] = {
        {0.0f, optics_gfx_color_rgba(255, 0, 0, 255)},
        {1.0f, optics_gfx_color_rgba(0, 0, 255, 255)},
    };
    optics_gfx_brush g = optics_gfx_brush_linear_gradient(
        (optics_gfx_point){0, 0}, (optics_gfx_point){(float)W, 0}, stops, 2);
    optics_gfx_canvas_fill_rect(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H}, &g);
}

static void draw_stroke(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_path *p = user;
    optics_gfx_brush paint = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_stroke_style paint_stroke = {.width = 8.0f};
    optics_gfx_canvas_stroke_path(canvas, p, &paint_stroke, &paint);
}

static void draw_donut(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_path *p = user;
    optics_gfx_brush paint = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_path(canvas, p, OPTICS_GFX_FILL_NON_ZERO, &paint);
}

static void draw_even_odd_path(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_path *p = user;
    optics_gfx_brush paint = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_fill_rule paint_rule = OPTICS_GFX_FILL_EVEN_ODD;
    optics_gfx_canvas_fill_path(canvas, p, paint_rule, &paint);
}

static void draw_glyph_run(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_canvas_draw_glyph_run(canvas, user);
}

static void draw_batchable_rects(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t x = (i % 8u) * 8u;
        uint32_t y = (i / 8u) * 8u;
        optics_gfx_color color = (i & 1u) ? optics_gfx_color_rgba(220, 40, 80, 255)
                                          : optics_gfx_color_rgba(40, 160, 220, 255);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){x, y, 8, 8}, color);
    }
}

/* Point-field shape: many small solid-colour dots (the wavora visual stage
 * workload). Every dot's draw-visible state except its vertices is
 * identical, so the whole field must collapse to a single vkCmdDraw — this
 * is the batching contract that keeps a 1536-dot field from costing 1536
 * draws and stalling the desktop compositor. */
static void draw_point_field(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    for (uint32_t i = 0; i < 256; ++i) {
        float x = (float)(i % 16u) * 8.0f + 4.0f;
        float y = (float)(i / 16u) * 8.0f + 4.0f;
        optics_gfx_color color = (i & 1u) ? optics_gfx_color_rgba(200, 60, 60, 255)
                                          : optics_gfx_color_rgba(60, 200, 120, 255);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){x - 2, y - 2, 4, 4}, color);
    }
}

typedef struct image_transform_case {
    optics_gfx_image *image;
    optics_gfx_sampler *sampler;
    optics_gfx_image_style paint;
} image_transform_case;

typedef struct image_record_case {
    image_transform_case image;
    optics_gfx_canvas_cache_entry record;
} image_record_case;

static void draw_rotated_image(optics_gfx_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    optics_gfx_canvas_save(canvas);
    optics_gfx_canvas_translate(canvas,
                                &(optics_gfx_canvas_translate_opts){.x = W / 2.0f, .y = H / 2.0f});
    optics_gfx_canvas_rotate(canvas, 1.57079632679f);
    optics_gfx_canvas_draw_image_sampled(canvas, tc->image, tc->sampler,
                                         (optics_gfx_rect){-32.0f, -16.0f, 64.0f, 32.0f},
                                         &tc->paint);
    optics_gfx_canvas_restore(canvas);
}

static void draw_round_image(optics_gfx_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    optics_gfx_canvas_draw_image_rrect(canvas, tc->image, (optics_gfx_rect){32, 32, 64, 64}, 32.0f,
                                       nullptr);
}

static void draw_image_through_independent_round_clip(optics_gfx_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    optics_gfx_canvas_draw_image_clipped_rrect(canvas, tc->image, (optics_gfx_rect){16, 16, 96, 96},
                                               (optics_gfx_rect){32, 32, 64, 64}, 16.0f,
                                               &tc->paint);
}

static void draw_combined_image(optics_gfx_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    optics_gfx_geometry g = optics_gfx_geom_rect((optics_gfx_rect){16, 16, 96, 96});
    optics_gfx_brush b = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image =
            {
                .image = tc->image,
                .sampler = tc->sampler,
                .src_rect = {0, 0, 0.5f, 0.5f},
                .clip_rect = {32, 32, 64, 64},
                .clip_radius = 16,
                .opaque_only = true,
                .tint = 0xFFFFFFFF,
            },
    };
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

static void draw_save_layer_opacity_group(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    /* Isolated opacity group (ADR-0091) on GPU Vulkan backend:
     * Two overlapping fully-opaque red rectangles inside a 0.5 opacity layer.
     * Overlap must NOT produce double-blend artifact. */
    optics_gfx_canvas_save_layer(
        canvas, &(optics_gfx_canvas_save_layer_opts){.bounds = nullptr, .opacity = 0.5f});
    optics_gfx_color red = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){8.0f, 8.0f, 24.0f, 24.0f}, red);
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){20.0f, 8.0f, 24.0f, 24.0f}, red);
    optics_gfx_canvas_restore(canvas);
}

static void draw_opaque_image(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_canvas_draw_image_opaque(canvas, user, (optics_gfx_rect){0, 0, (float)W, (float)H});
}

static void record_rotated_image(optics_gfx_canvas *canvas, void *user) {
    image_record_case *tc = user;
    OT_EXPECT(optics_gfx_canvas_cache_begin(canvas));
    draw_rotated_image(canvas, &tc->image);
    tc->record = optics_gfx_canvas_cache_end(canvas);
    OT_EXPECT(tc->record.slot != nullptr);
}

static void replay_record(optics_gfx_canvas *canvas, void *user) {
    OT_EXPECT(optics_gfx_canvas_cache_replay(canvas, *(optics_gfx_canvas_cache_entry *)user));
}

#if defined(OPTICS_GFX_TEXT_HAVE_FTHB)
typedef struct text_family_case {
    optics_text_ctx *text;
    optics_gfx_arena *arena;
    optics_text_family family;
    float y;
} text_family_case;

static void draw_text_family(optics_gfx_canvas *canvas, void *user) {
    text_family_case *tc = user;
    const char *s = "Visible";
    optics_text_style style = {
        .size_px = 24.0f,
        .weight = 400.0f,
        .color = optics_gfx_color_rgba(255, 255, 255, 255),
        .family = tc->family,
    };
    optics_text_draw(tc->text, &(optics_text_draw_opts){.canvas = canvas,
                                                        .arena = tc->arena,
                                                        .x = 12.0f,
                                                        .y = tc->y,
                                                        .utf8 = s,
                                                        .len = strlen(s),
                                                        .style = &style});
}

static bool region_has_ink(const uint8_t *px, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            const uint8_t *p = px_at(px, x, y);
            if (p[0] > 32 || p[1] > 32 || p[2] > 32)
                return true;
        }
    }
    return false;
}
#endif

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_render: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    }
    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
    }

    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 64 * 1024, nullptr) == OPTICS_GFX_OK);

    static uint8_t px[BYTES];

    /* --- state-identical submit batching.
     * Solid colours live in vertices, so 64 adjacent rect submissions share
     * all draw-visible Vulkan state and must collapse to one vkCmdDraw while
     * preserving every submitted primitive. ADR-0069 adds one output-transform
     * blit per pass (CLEAR pass: no seed blit), so recorded draws are 1+1. --- */
    {
        uint64_t submits = optics_gfx_canvas_submit_calls(canvas);
        uint64_t draws = optics_gfx_canvas_recorded_draws(canvas);
        OT_EXPECT(render_frame(s, canvas, draw_batchable_rects, nullptr) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_submit_calls(canvas) - submits == 64);
        OT_EXPECT(optics_gfx_canvas_recorded_draws(canvas) - draws == 2);

        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 4, 4)[2] > 180);   /* first blue tile */
        OT_EXPECT(px_at(px, 60, 60)[0] > 180); /* last red tile */
    }

    /* --- point-field batching (the wavora stage workload): 256 small dots
     * in one draw, pixels land, and alternating colours resolve. ADR-0069's
     * output blit adds the usual +1. --- */
    {
        uint64_t submits = optics_gfx_canvas_submit_calls(canvas);
        uint64_t draws = optics_gfx_canvas_recorded_draws(canvas);
        OT_EXPECT(render_frame(s, canvas, draw_point_field, nullptr) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_submit_calls(canvas) - submits == 256);
        OT_EXPECT(optics_gfx_canvas_recorded_draws(canvas) - draws == 2);

        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 4, 4)[1] > 150);     /* i=0 green: centre (4,4)  */
        OT_EXPECT(px_at(px, 12, 4)[0] > 150);    /* i=1 red: centre (12,4)   */
        OT_EXPECT(px_at(px, 4, H - 4)[1] > 150); /* i=240 green: centre(4,124)*/
    }

    /* --- unknown pNext pass extension is ignored cleanly, and even-odd path
     * fills operate natively without any stencil attachment --- */
    {
        OT_EXPECT(render_frame_unknown_ext(s, canvas, draw_batchable_rects, nullptr) ==
                  OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 4, 4)[2] > 180);
        OT_EXPECT(px_at(px, 60, 60)[0] > 180);
        OT_EXPECT(px_at(px, W - 2, H - 2)[0] < 20);
    }

    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_rect(p, (optics_gfx_rect){16, 16, 64, 64});
        OT_EXPECT(render_frame_unknown_ext(s, canvas, draw_even_odd_path, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 32, 32)[0] > 200);
        optics_gfx_arena_reset(&arena);
    }

    /* --- linear gradient: red → blue across x --- */
    {
        OT_EXPECT(render_frame(s, canvas, draw_linear_gradient, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *left = px_at(px, 2, H / 2);
        const uint8_t *mid = px_at(px, W / 2, H / 2);
        const uint8_t *right = px_at(px, W - 3, H / 2);
        /* ADR-0069: stops interpolate in linear light. One pixel in from an
         * end (t = 2.5/128) the off channel is 0.0195 linear, which
         * sRGB-encodes to ~38 (gamma-space interpolation gave ~5); the
         * midpoint is 0.5 linear per channel -> ~187, not 128. Dither adds
         * ±1 LSB on top of the reference values. */
        OT_EXPECT(left[0] > 220 && left[2] > 30 && left[2] < 48);    /* red end */
        OT_EXPECT(right[2] > 220 && right[0] > 30 && right[0] < 48); /* blue end */
        OT_EXPECT(mid[0] > 183 && mid[0] < 192 && mid[2] > 183 && mid[2] < 192);
        OT_EXPECT(left[0] > mid[0] && mid[0] > right[0]);
        OT_EXPECT(right[2] > mid[2] && mid[2] > left[2]);
    }

    /* --- stroke: 8px horizontal line through the centre --- */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_move_to(p, 16.0f, H / 2.0f);
        optics_gfx_path_line_to(p, W - 16.0f, H / 2.0f);

        OT_EXPECT(render_frame(s, canvas, draw_stroke, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* On the line: white. 8 px above it: background. Before the
         * butt cap at x=16: background. */
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        OT_EXPECT(px_at(px, W / 2, H / 2 - 8)[0] < 20);
        OT_EXPECT(px_at(px, W / 2, H / 2 + 8)[0] < 20);
        OT_EXPECT(px_at(px, 8, H / 2)[0] < 20);
    }

    /* --- multi-subpath stroke: two separate lines in one path --- *
     * A move_to starts a new subpath; the stroker must render every
     * subpath, not just the first (regression: glyphs like a settings
     * gear or a sidebar with a divider lost all but their first
     * subpath). */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_move_to(p, 16.0f, H / 2.0f - 24.0f); /* subpath 1 */
        optics_gfx_path_line_to(p, W - 16.0f, H / 2.0f - 24.0f);
        optics_gfx_path_move_to(p, 16.0f, H / 2.0f + 24.0f); /* subpath 2 */
        optics_gfx_path_line_to(p, W - 16.0f, H / 2.0f + 24.0f);

        OT_EXPECT(render_frame(s, canvas, draw_stroke, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Both lines must be white; the gap between them background. */
        OT_EXPECT(px_at(px, W / 2, H / 2 - 24)[0] == 255); /* subpath 1 */
        OT_EXPECT(px_at(px, W / 2, H / 2 + 24)[0] == 255); /* subpath 2 */
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] < 20);        /* gap */
    }

    /* --- donut fill: outer circle + reverse-wound inner hole --- */
    {
        const float cx = W / 2.0f, cy = H / 2.0f;
        const float r_out = 48.0f, r_in = 20.0f;

        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_circle(p, cx, cy, r_out);
        /* Inner contour wound opposite to flux_path_add_circle's
         * direction → a hole per ADR-0011. */
        optics_gfx_path_move_to(p, cx + r_in, cy);
        for (int i = 1; i < 32; ++i) {
            float a = -(float)i / 32.0f * 6.28318530718f;
            optics_gfx_path_line_to(p, cx + r_in * cosf(a), cy + r_in * sinf(a));
        }
        optics_gfx_path_close(p);

        OT_EXPECT(render_frame(s, canvas, draw_donut, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Centre of the hole: background. Ring interior (between the
         * radii, four directions): white. Outside the outer circle:
         * background. ADR-0069: the output dither costs up to 1 LSB, so
         * full-white reads back as 254/255, not exactly 255. */
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] < 20);
        uint32_t ring = (uint32_t)((r_in + r_out) / 2.0f);
        OT_EXPECT(px_at(px, W / 2 + ring, H / 2)[0] >= 253);
        OT_EXPECT(px_at(px, W / 2 - ring, H / 2)[0] >= 253);
        OT_EXPECT(px_at(px, W / 2, H / 2 + ring)[0] >= 253);
        OT_EXPECT(px_at(px, W / 2, H / 2 - ring)[0] >= 253);
        OT_EXPECT(px_at(px, 2, 2)[0] < 20);
    }

    /* --- self-intersecting pentagram: nonzero analytic coverage --- */
    {
        const float cx = W / 2.0f, cy = H / 2.0f, R = 52.0f;
        optics_gfx_point v[5];
        for (int i = 0; i < 5; ++i) {
            float a = (float)i * (6.28318530718f / 5.0f) - 1.57079632679f;
            v[i] = (optics_gfx_point){cx + R * cosf(a), cy + R * sinf(a)};
        }
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_move_to(p, v[0].x, v[0].y);
        for (int i = 1; i < 5; ++i) {
            int k = (i * 2) % 5; /* 0 → 2 → 4 → 1 → 3 → close */
            optics_gfx_path_line_to(p, v[k].x, v[k].y);
        }
        optics_gfx_path_close(p);

        OT_EXPECT(render_frame(s, canvas, draw_donut, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Centre (inside the inner pentagon, winding count 2): filled. */
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        /* Inside the top point's spike (winding 1): filled. */
        uint32_t spike_y = (uint32_t)(cy - R) + 6;
        OT_EXPECT(px_at(px, W / 2, spike_y)[0] == 255);
        /* Notch bisector between the top and upper-right spikes
         * (angle -54°, radius 0.7R; the boundary there is the inner
         * vertex at ~0.38R): outside the star body, background. */
        OT_EXPECT(px_at(px, (uint32_t)(cx + 0.7f * R * 0.5878f),
                        (uint32_t)(cy - 0.7f * R * 0.8090f))[0] < 20);
        /* Far corner: background. */
        OT_EXPECT(px_at(px, 2, 2)[0] < 20);

        /* Repeating across frames must remain clean without accumulated state. */
        OT_EXPECT(render_frame(s, canvas, draw_donut, p) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        OT_EXPECT(px_at(px, 2, 2)[0] < 20);
    }

    /* --- affine image transform + paint opacity --- */
    {
        /* A 2×2 quadrant image, rotated 90 degrees. Regression coverage for
         * per-vertex image UVs: the former screen-AABB UV reconstruction
         * sampled the wrong quadrant after rotation. A half-alpha white
         * paint simultaneously verifies image tint/opacity modulation. */
        uint32_t image_px[4] = {
            0xFF0000FFu,
            0xFF00FF00u, /* red, green */
            0xFFFFFFFFu,
            0xFFFF0000u, /* white, blue */
        };
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.width = 2;
        idesc.height = 2;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = image_px;
        optics_gfx_image *image = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &image) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_image_size_bytes(image) == 2u * 2u * 4u);

        /* ADR-0090 pinned-content budget: the list owns the image bytes, and a
         * budget smaller than one image refuses publication instead of
         * silently pinning it. */
        optics_gfx_encoder *enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
        optics_gfx_brush image_brush = {
            .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
            .blend = OPTICS_GFX_BLEND_SRC_OVER,
            .opacity = 1.0f,
            .image = {.image = image, .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
        };
        optics_gfx_geometry image_geom = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 2, 2});
        optics_gfx_encoder_draw_geometry(enc, &(optics_gfx_encoder_draw_geometry_opts){
                                                  .geom = &image_geom, .brush = &image_brush});
        optics_gfx_display_list *image_list = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(enc, &image_list) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(enc);
        OT_EXPECT(optics_gfx_display_list_retained_bytes(image_list) == 16u);
        optics_gfx_display_list_release(image_list);

        OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){.max_retained_bytes = 8u},
                                            &enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_draw_geometry(enc, &(optics_gfx_encoder_draw_geometry_opts){
                                                  .geom = &image_geom, .brush = &image_brush});
        OT_EXPECT(optics_gfx_encoder_status(enc) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(optics_gfx_encoder_finish(enc, &image_list) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(image_list == nullptr);
        optics_gfx_encoder_destroy(enc);

        /* ADR-0090 immutable snapshot: a list recorded against a snapshot
         * keeps the old pixels after the original image is updated. */
        optics_gfx_image *snapshot = nullptr;
        OT_EXPECT(optics_gfx_image_snapshot(image, &snapshot) == OPTICS_GFX_OK);
        OT_EXPECT(snapshot != nullptr);
        optics_gfx_encoder *snapshot_enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &snapshot_enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_draw_geometry(
            snapshot_enc,
            &(optics_gfx_encoder_draw_geometry_opts){
                .geom = &image_geom,
                .brush = &(optics_gfx_brush){
                    .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                    .blend = OPTICS_GFX_BLEND_SRC_OVER,
                    .opacity = 1.0f,
                    .image = {.image = snapshot,
                              .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
                }});
        optics_gfx_display_list *old_list = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(snapshot_enc, &old_list) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(snapshot_enc);

        uint32_t blue_px[4] = {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u};
        OT_EXPECT(optics_gfx_image_update_region(
                      image, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                    .y = 0,
                                                                    .width = 2,
                                                                    .height = 2,
                                                                    .data = blue_px,
                                                                    .bytes = sizeof blue_px}) ==
                  OPTICS_GFX_OK);

        optics_gfx_encoder *new_enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &new_enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_draw_geometry(
            new_enc, &(optics_gfx_encoder_draw_geometry_opts){
                         .geom = &image_geom,
                         .brush = &(optics_gfx_brush){
                             .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                             .blend = OPTICS_GFX_BLEND_SRC_OVER,
                             .opacity = 1.0f,
                             .image = {.image = image,
                                       .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
                         }});
        optics_gfx_display_list *new_list = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(new_enc, &new_list) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(new_enc);

        optics_gfx_geometry full =
            optics_gfx_geom_rect((optics_gfx_rect){0, 0, (float)W, (float)H});
        (void)full;
        display_list_case old_case = {.list = old_list};
        /* Scale the 2x2 source over the output so both colours are visible. */
        optics_gfx_encoder *scale_enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &scale_enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_scale(scale_enc, &(optics_gfx_encoder_scale_opts){
                                                .sx = (float)W / 2.0f, .sy = (float)H / 2.0f});
        optics_gfx_encoder_draw_geometry(
            scale_enc, &(optics_gfx_encoder_draw_geometry_opts){
                           .geom = &image_geom,
                           .brush = &(optics_gfx_brush){
                               .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                               .blend = OPTICS_GFX_BLEND_SRC_OVER,
                               .opacity = 1.0f,
                               .image = {.image = snapshot,
                                         .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
                           }});
        optics_gfx_display_list *scaled_old = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(scale_enc, &scaled_old) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(scale_enc);
        old_case.list = scaled_old;
        OT_EXPECT(render_frame(s, canvas, draw_display_list, &old_case) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 32, 32)[0] > 200 && px_at(px, 32, 32)[1] < 40);
        OT_EXPECT(px_at(px, 96, 32)[1] > 200 && px_at(px, 96, 32)[0] < 40);

        optics_gfx_encoder *scale_new_enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &scale_new_enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_scale(scale_new_enc, &(optics_gfx_encoder_scale_opts){
                                                    .sx = (float)W / 2.0f, .sy = (float)H / 2.0f});
        optics_gfx_encoder_draw_geometry(
            scale_new_enc,
            &(optics_gfx_encoder_draw_geometry_opts){
                .geom = &image_geom,
                .brush = &(optics_gfx_brush){
                    .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                    .blend = OPTICS_GFX_BLEND_SRC_OVER,
                    .opacity = 1.0f,
                    .image = {.image = image,
                              .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
                }});
        optics_gfx_display_list *scaled_new = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(scale_new_enc, &scaled_new) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(scale_new_enc);
        display_list_case new_case = {.list = scaled_new};
        OT_EXPECT(render_frame(s, canvas, draw_display_list, &new_case) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 32, 32)[2] > 200 && px_at(px, 32, 32)[0] < 40);
        OT_EXPECT(px_at(px, 96, 96)[2] > 200 && px_at(px, 96, 96)[0] < 40);

        optics_gfx_display_list_release(old_list);
        optics_gfx_display_list_release(new_list);
        optics_gfx_display_list_release(scaled_old);
        optics_gfx_display_list_release(scaled_new);
        optics_gfx_image_release(snapshot);

        /* Restore the quadrant fixture for the transform tests below. */
        OT_EXPECT(optics_gfx_image_update_region(
                      image, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                    .y = 0,
                                                                    .width = 2,
                                                                    .height = 2,
                                                                    .data = image_px,
                                                                    .bytes = sizeof image_px}) ==
                  OPTICS_GFX_OK);

        /* Color-managed images share their immutable color state across a
         * snapshot/COW split instead of refusing the update. */
        optics_gfx_color_space p3 = OPTICS_GFX_COLOR_SPACE_DISPLAY_P3;
        optics_gfx_image_color_space_desc color_desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC,
            .space = &p3,
        };
        optics_gfx_image_desc color_image_desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .next = &color_desc,
            .width = 2,
            .height = 2,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
            .initial_data = image_px,
        };
        optics_gfx_image *color_image = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &color_image_desc, &color_image) == OPTICS_GFX_OK);
        optics_gfx_image *color_snapshot = nullptr;
        OT_EXPECT(optics_gfx_image_snapshot(color_image, &color_snapshot) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_image_update_region(color_image,
                                                 &(optics_gfx_image_update_region_opts){
                                                     .x = 0,
                                                     .y = 0,
                                                     .width = 2,
                                                     .height = 2,
                                                     .data = blue_px,
                                                     .bytes = sizeof blue_px}) == OPTICS_GFX_OK);
        optics_gfx_image_release(color_snapshot);
        optics_gfx_image_release(color_image);

        /* DisplayList serialization carries the image as a caller-owned ID,
         * then rebinds it through the resolver on load (ADR-0157). */
        optics_gfx_encoder *wire_enc = nullptr;
        OT_EXPECT(optics_gfx_encoder_create(nullptr, &wire_enc) == OPTICS_GFX_OK);
        optics_gfx_encoder_draw_geometry(
            wire_enc, &(optics_gfx_encoder_draw_geometry_opts){
                          .geom = &(optics_gfx_geometry){.kind = OPTICS_GFX_GEOM_RECT,
                                                         .rect = {.rect = {0, 0, 2, 2}}},
                          .brush = &image_brush});
        optics_gfx_display_list *wire_list = nullptr;
        OT_EXPECT(optics_gfx_encoder_finish(wire_enc, &wire_list) == OPTICS_GFX_OK);
        optics_gfx_encoder_destroy(wire_enc);
        uint8_t *wire = nullptr;
        size_t wire_size = 0;
        optics_gfx_display_list_serialize_opts sopts = OPTICS_GFX_DISPLAY_LIST_SERIALIZE_OPTS_INIT;
        sopts.resource_id = test_resource_id;
        sopts.user = image;
        sopts.out_data = &wire;
        sopts.out_size = &wire_size;
        OT_EXPECT(optics_gfx_display_list_serialize(wire_list, &sopts) == OPTICS_GFX_OK);
        optics_gfx_display_list *wire_loaded = nullptr;
        optics_gfx_display_list_deserialize_opts dopts =
            OPTICS_GFX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
        dopts.data = wire;
        dopts.size = wire_size;
        dopts.resolve = test_resource_resolve;
        dopts.user = image;
        dopts.out_list = &wire_loaded;
        OT_EXPECT(optics_gfx_display_list_deserialize(&dopts) == OPTICS_GFX_OK);
        display_list_case wire_case = {.list = wire_loaded};
        OT_EXPECT(render_frame(s, canvas, draw_display_list, &wire_case) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 32, 32)[3] == 255);
        optics_gfx_display_list_serialized_free(wire);
        optics_gfx_display_list_release(wire_loaded);
        optics_gfx_display_list_release(wire_list);

        optics_gfx_sampler_desc sdesc = OPTICS_GFX_SAMPLER_DESC_INIT;
        sdesc.min_filter = OPTICS_GFX_FILTER_NEAREST;
        sdesc.mag_filter = OPTICS_GFX_FILTER_NEAREST;
        sdesc.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        optics_gfx_sampler *nearest = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sdesc, &nearest) == OPTICS_GFX_OK);

        image_record_case tc = {
            .image =
                {
                    .image = image,
                    .sampler = nearest,
                    .paint = {.tint = optics_gfx_color_rgba_premul(255, 255, 255, 128),
                              .opacity = 1.0f},
                },
            .record = OPTICS_GFX_CANVAS_CACHE_ENTRY_INIT,
        };
        OT_EXPECT(render_frame(s, canvas, draw_combined_image, &tc.image) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 48, 48)[0] > 245);
        OT_EXPECT(px_at(px, 80, 80)[0] > 245);
        OT_EXPECT(px_at(px, 80, 80)[1] < 8 && px_at(px, 80, 80)[2] < 8);
        OT_EXPECT(px_at(px, 24, 64)[0] < 8);
        OT_EXPECT(px_at(px, 33, 33)[0] < 8);
        OT_EXPECT(render_frame(s, canvas, draw_round_image, &tc.image) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 64, 64)[0] > 80); /* image centre survives */
        OT_EXPECT(px_at(px, 33, 33)[0] < 8);  /* square corner is clipped */

        OT_EXPECT(render_frame(s, canvas, draw_image_through_independent_round_clip, &tc.image) ==
                  OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(px_at(px, 64, 64)[0] > 40); /* destination content survives */
        OT_EXPECT(px_at(px, 24, 64)[0] < 8);  /* inside dst, outside shared clip */
        OT_EXPECT(px_at(px, 33, 33)[0] < 8);  /* shared clip has analytic corners */

        OT_EXPECT(render_frame(s, canvas, record_rotated_image, &tc) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *top_right = px_at(px, 72, 48);    /* source top-left: red */
        const uint8_t *bottom_right = px_at(px, 72, 80); /* source top-right: green */
        const uint8_t *bottom_left = px_at(px, 56, 80);  /* source bottom-right: blue */
        const uint8_t *top_left = px_at(px, 56, 48);     /* source bottom-left: white */
        /* ADR-0069: the half-alpha white tint multiplies in linear light, so
         * the lit channels are 0.502 linear over black, which sRGB-encodes
         * to ~188 (gamma-space blending gave 128). ±4 absorbs the output
         * dither and quantisation. */
        OT_EXPECT(top_right[0] > 184 && top_right[0] < 192 && top_right[1] < 8 && top_right[2] < 8);
        OT_EXPECT(bottom_right[0] < 8 && bottom_right[1] > 184 && bottom_right[1] < 192 &&
                  bottom_right[2] < 8);
        OT_EXPECT(bottom_left[0] < 8 && bottom_left[1] < 8 && bottom_left[2] > 184 &&
                  bottom_left[2] < 192);
        OT_EXPECT(top_left[0] > 184 && top_left[0] < 192 && top_left[1] > 184 &&
                  top_left[1] < 192 && top_left[2] > 184 && top_left[2] < 192);
        OT_EXPECT(px_at(px, 40, 64)[0] < 8); /* outside the rotated 32×64 quad */

        /* A display-list segment owns both referenced bindless resources.
         * Drop the caller's image and custom-sampler references, then replay:
         * recycled handles would corrupt these pixels or trip validation. */
        uint8_t live[BYTES];
        memcpy(live, px, sizeof live);
        optics_gfx_sampler_release(nearest);
        optics_gfx_image_release(image);
        OT_EXPECT(render_frame(s, canvas, replay_record, &tc.record) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        OT_EXPECT(memcmp(live, px, sizeof live) == 0);
        optics_gfx_canvas_cache_release(canvas, tc.record);
    }

    /* --- alpha-free image import ignores the undefined X channel --- */
    {
        const uint8_t xrgb_px[4] = {37, 113, 211, 0};
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = xrgb_px;
        optics_gfx_image *image = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &image) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame_unknown_ext(s, canvas, draw_opaque_image, image) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        /* ADR-0069: the UNORM texel is sRGB-decoded on sampling and
         * re-encoded at output; the round trip is the identity up to the
         * ±1 LSB output dither and quantisation. */
        OT_EXPECT(centre[0] >= 35 && centre[0] <= 39);
        OT_EXPECT(centre[1] >= 111 && centre[1] <= 115);
        OT_EXPECT(centre[2] >= 209 && centre[2] <= 213);
        OT_EXPECT(centre[3] == 255);
        optics_gfx_image_release(image);
    }

    /* --- batched glyph run (ADR-0010): two quads, one atlas, one
     * draw. Atlas 8×4 R8: left 4×4 block full coverage, right block
     * quarter coverage. Quad A samples the left block with red tint;
     * quad B samples the right block with white tint. --- */
    {
        uint8_t atlas_px[8 * 4];
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                atlas_px[y * 8 + x] = (x < 4) ? 255u : 64u;

        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.width = 8;
        idesc.height = 4;
        idesc.format = OPTICS_GFX_FORMAT_R8_UNORM;
        idesc.initial_data = atlas_px;
        optics_gfx_image *atlas = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &atlas) == OPTICS_GFX_OK);

        optics_gfx_sampler_desc sdesc = OPTICS_GFX_SAMPLER_DESC_INIT;
        sdesc.min_filter = OPTICS_GFX_FILTER_NEAREST;
        sdesc.mag_filter = OPTICS_GFX_FILTER_NEAREST;
        sdesc.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        optics_gfx_sampler *nearest = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sdesc, &nearest) == OPTICS_GFX_OK);

        optics_gfx_glyph_quad quads[2] = {
            {.sx = 16,
             .sy = 16,
             .sw = 24,
             .sh = 24,
             .ax = 0,
             .ay = 0,
             .aw = 4,
             .ah = 4,
             .color = optics_gfx_color_rgba(255, 0, 0, 255)},
            {.sx = 72,
             .sy = 16,
             .sw = 24,
             .sh = 24,
             .ax = 4,
             .ay = 0,
             .aw = 4,
             .ah = 4,
             .color = optics_gfx_color_rgba(255, 255, 255, 255)},
        };
        optics_gfx_glyph_run_desc run = OPTICS_GFX_GLYPH_RUN_DESC_INIT;
        run.atlas = atlas;
        run.sampler = nearest;
        run.quads = quads;
        run.quad_count = 2;

        OT_EXPECT(render_frame(s, canvas, draw_glyph_run, &run) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Quad A centre: full coverage × red tint = pure red. */
        const uint8_t *a = px_at(px, 28, 28);
        OT_EXPECT(a[0] > 240 && a[1] < 12 && a[2] < 12);

        /* Quad B centre: quarter coverage × white tint. ADR-0069: coverage
         * scales in linear light — 64/255 ≈ 0.251 linear over black, which
         * sRGB-encodes to ~137 (gamma-space blending gave ~64). */
        const uint8_t *b = px_at(px, 84, 28);
        OT_EXPECT(b[0] > 128 && b[0] < 148);
        OT_EXPECT(b[1] > 128 && b[1] < 148);

        /* Between the quads and outside them: background. */
        OT_EXPECT(px_at(px, 56, 28)[0] < 20);
        OT_EXPECT(px_at(px, 28, 60)[0] < 20);

        optics_gfx_sampler_release(nearest);
        optics_gfx_image_release(atlas);
    }

#if defined(OPTICS_GFX_TEXT_HAVE_FTHB)
    /* --- public text draw honours every family slot.
     * Regression: txt_ensure_face_loaded accepted only the two weight
     * slots for sans-serif, so serif/mono shaped to zero glyphs. wisp pins
     * fenced code blocks to MONO, which made code backgrounds render while
     * the actual code vanished. --- */
    {
        optics_text_ctx *text = nullptr;
        optics_text_desc td = {.device = d, .scale = 1.0f};
        OT_EXPECT(optics_text_create(&td, &text) == OPTICS_GFX_OK);
        OT_EXPECT(text != nullptr);

        text_family_case cases[3] = {
            {text, &arena, OPTICS_GFX_TEXT_FAMILY_SANS, 12.0f},
            {text, &arena, OPTICS_GFX_TEXT_FAMILY_SERIF, 50.0f},
            {text, &arena, OPTICS_GFX_TEXT_FAMILY_MONO, 88.0f},
        };

        for (int i = 0; i < 3; i++) {
            const char *text_s = "Visible";
            optics_text_style style = {
                .size_px = 24.0f,
                .weight = 400.0f,
                .color = optics_gfx_color_rgba(255, 255, 255, 255),
                .family = cases[i].family,
            };
            optics_text_metrics m = optics_text_measure(
                text, &(optics_text_measure_opts){
                          .utf8 = text_s, .len = strlen(text_s), .style = &style});
            OT_EXPECT(m.width > 0.0f);
            OT_EXPECT(m.height > 0.0f);

            OT_EXPECT(render_frame(s, canvas, draw_text_family, &cases[i]) == OPTICS_GFX_OK);
            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            OT_EXPECT(region_has_ink(px, 10, (uint32_t)cases[i].y, 120, (uint32_t)cases[i].y + 32));
        }

        optics_text_release(text);
    }
#endif

    /* ADR-0091: Vulkan GPU isolated opacity group verification */
    OT_EXPECT(render_frame(s, canvas, draw_save_layer_opacity_group, nullptr) == OPTICS_GFX_OK);
    memset(px, 0, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
    const uint8_t *px_single = px_at(px, 12, 16);
    const uint8_t *px_overlap = px_at(px, 24, 16);
    OT_EXPECT(abs((int)px_single[0] - 188) <= 4);
    OT_EXPECT(abs((int)px_overlap[0] - 188) <= 4);
    OT_EXPECT(abs((int)px_single[0] - (int)px_overlap[0]) <= 2);
    OT_EXPECT(px_single[3] == 255);
    OT_EXPECT(px_overlap[3] == 255);

    /* ADR-0090: snapshot and content update serialize on the image version
     * lock; a concurrent snapshot must never observe a half-swapped handle. */
    {
        uint32_t source[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.width = 2;
        idesc.height = 2;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = source;
        optics_gfx_image *image = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &image) == OPTICS_GFX_OK);
        snapshot_stress stress = {.image = image};
        atomic_init(&stress.stop, false);
        atomic_init(&stress.failures, 0u);
        pthread_t thread;
        OT_EXPECT(pthread_create(&thread, nullptr, snapshot_worker, &stress) == 0);
        for (uint32_t i = 0; i < 200; ++i) {
            source[0] = 0xFF000000u | i;
            OT_EXPECT(optics_gfx_image_update_region(
                          image, &(optics_gfx_image_update_region_opts){.x = 0,
                                                                        .y = 0,
                                                                        .width = 2,
                                                                        .height = 2,
                                                                        .data = source,
                                                                        .bytes = sizeof source}) ==
                      OPTICS_GFX_OK);
        }
        atomic_store_explicit(&stress.stop, true, memory_order_relaxed);
        pthread_join(thread, nullptr);
        OT_EXPECT(atomic_load_explicit(&stress.failures, memory_order_relaxed) == 0u);
        optics_gfx_image_release(image);
    }

    optics_gfx_arena_deinit(&arena);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
