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
#include <flux/flux.h>
#include <flux/vulkan.h>
#if defined(FLUX_TEXT_HAVE_FTHB)
#include <flux-text/text.h>
#endif
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u
#define BYTES (W * H * 4u)

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

typedef void (*draw_fn)(flux_canvas *canvas, void *user);

static flux_result render_frame(flux_surface *s, flux_canvas *canvas, draw_fn draw, void *user) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;

    flux_color clear = flux_color_rgba(0, 0, 0, 255);
    r = flux_canvas_begin(canvas, frame, &clear);
    if (r != FLUX_OK)
        return r;
    draw(canvas, user);
    flux_canvas_end(canvas);

    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    return flux_frame_present(frame);
}

static flux_result render_frame_no_stencil(flux_surface *s, flux_canvas *canvas, draw_fn draw,
                                           void *user) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;

    flux_color clear = flux_color_rgba(0, 0, 0, 255);
    flux_canvas_pass_desc pass = FLUX_CANVAS_PASS_DESC_INIT;
    pass.clear_color = &clear;
    pass.antialias = FLUX_CANVAS_ANTIALIAS_NONE;
    flux_canvas_no_stencil_desc no_stencil = FLUX_CANVAS_NO_STENCIL_DESC_INIT;
    no_stencil.enabled = true;
    /* Unknown pNext entries are skipped for forward compatibility. Putting
     * one before the known extension exercises real chained parsing rather
     * than only the one-node happy path. */
    const struct {
        flux_struct_type type;
        const void *next;
    } future_extension = {FLUX_TYPE_UNKNOWN, &no_stencil};
    pass.next = &future_extension;
    r = flux_canvas_begin_pass(canvas, frame, &pass);
    if (r != FLUX_OK)
        return r;
    draw(canvas, user);
    flux_result pass_result = flux_canvas_end_checked(canvas);

    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    r = flux_frame_present(frame);
    return pass_result != FLUX_OK ? pass_result : r;
}

/* --- draw callbacks ------------------------------------------------ */

static void draw_linear_gradient(flux_canvas *canvas, void *user) {
    (void)user;
    flux_gradient_stop stops[2] = {
        {0.0f, flux_color_rgba(255, 0, 0, 255)},
        {1.0f, flux_color_rgba(0, 0, 255, 255)},
    };
    flux_paint g =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 2);
    flux_canvas_fill_rect(canvas, (flux_rect){0, 0, (float)W, (float)H}, &g);
}

static void draw_stroke(flux_canvas *canvas, void *user) {
    flux_path *p = user;
    flux_paint paint = flux_paint_solid(flux_color_rgba(255, 255, 255, 255));
    paint.stroke_width = 8.0f;
    flux_canvas_stroke_path(canvas, p, &paint);
}

static void draw_donut(flux_canvas *canvas, void *user) {
    flux_path *p = user;
    flux_paint paint = flux_paint_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_path(canvas, p, &paint);
}

static void draw_even_odd_path(flux_canvas *canvas, void *user) {
    flux_path *p = user;
    flux_paint paint = flux_paint_solid(flux_color_rgba(255, 255, 255, 255));
    paint.fill_rule = FLUX_FILL_EVEN_ODD;
    flux_canvas_fill_path(canvas, p, &paint);
}

static void draw_glyph_run(flux_canvas *canvas, void *user) {
    flux_canvas_draw_glyph_run(canvas, user);
}

static void draw_batchable_rects(flux_canvas *canvas, void *user) {
    (void)user;
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t x = (i % 8u) * 8u;
        uint32_t y = (i / 8u) * 8u;
        flux_color color = (i & 1u) ? flux_color_rgba(220, 40, 80, 255)
                                    : flux_color_rgba(40, 160, 220, 255);
        flux_canvas_fill_rect_color(canvas, (flux_rect){x, y, 8, 8}, color);
    }
}

typedef struct image_transform_case {
    flux_image *image;
    flux_sampler *sampler;
    flux_paint paint;
} image_transform_case;

typedef struct image_record_case {
    image_transform_case image;
    flux_canvas_record record;
} image_record_case;

static void draw_rotated_image(flux_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    flux_canvas_save(canvas);
    flux_canvas_translate(canvas, W / 2.0f, H / 2.0f);
    flux_canvas_rotate(canvas, 1.57079632679f);
    flux_canvas_draw_image_sampled(canvas, tc->image, tc->sampler,
                                   (flux_rect){-32.0f, -16.0f, 64.0f, 32.0f}, &tc->paint);
    flux_canvas_restore(canvas);
}

static void draw_round_image(flux_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    flux_canvas_draw_image_rrect(canvas, tc->image, (flux_rect){32, 32, 64, 64}, 32.0f,
                                 nullptr);
}

static void draw_image_through_independent_round_clip(flux_canvas *canvas, void *user) {
    image_transform_case *tc = user;
    flux_canvas_draw_image_clipped_rrect(canvas, tc->image, (flux_rect){16, 16, 96, 96},
                                         (flux_rect){32, 32, 64, 64}, 16.0f, &tc->paint);
}

static void draw_opaque_image(flux_canvas *canvas, void *user) {
    flux_canvas_draw_image_opaque(canvas, user, (flux_rect){0, 0, (float)W, (float)H});
}

static void record_rotated_image(flux_canvas *canvas, void *user) {
    image_record_case *tc = user;
    EXPECT(flux_canvas_begin_record(canvas));
    draw_rotated_image(canvas, &tc->image);
    tc->record = flux_canvas_end_record(canvas);
    EXPECT(tc->record.slot != nullptr);
}

static void replay_record(flux_canvas *canvas, void *user) {
    EXPECT(flux_canvas_replay(canvas, *(flux_canvas_record *)user));
}

#if defined(FLUX_TEXT_HAVE_FTHB)
typedef struct text_family_case {
    flux_text *text;
    flux_arena *arena;
    flux_text_family family;
    float y;
} text_family_case;

static void draw_text_family(flux_canvas *canvas, void *user) {
    text_family_case *tc = user;
    const char *s = "Visible";
    flux_text_style style = {
        .size_px = 24.0f,
        .weight = 400.0f,
        .color = flux_color_rgba(255, 255, 255, 255),
        .family = tc->family,
    };
    flux_text_draw(tc->text, canvas, tc->arena, 12.0f, tc->y, s, strlen(s), &style);
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
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_render: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    }
    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 64 * 1024, nullptr) == FLUX_OK);

    static uint8_t px[BYTES];

    /* --- state-identical submit batching.
     * Solid colours live in vertices, so 64 adjacent rect submissions share
     * all draw-visible Vulkan state and must collapse to one vkCmdDraw while
     * preserving every submitted primitive. --- */
    {
        uint64_t submits = flux_canvas_submit_calls(canvas);
        uint64_t draws = flux_canvas_recorded_draws(canvas);
        EXPECT(render_frame(s, canvas, draw_batchable_rects, nullptr) == FLUX_OK);
        EXPECT(flux_canvas_submit_calls(canvas) - submits == 64);
        EXPECT(flux_canvas_recorded_draws(canvas) - draws == 1);

        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, 4, 4)[2] > 180);   /* first blue tile */
        EXPECT(px_at(px, 60, 60)[0] > 180); /* last red tile */
    }

    /* --- true no-stencil pass: normal solid pipelines remain valid while
     * dynamic rendering binds no stencil attachment. This also exercises the
     * independent VK_FORMAT_UNDEFINED pipeline variant under validation. --- */
    {
        EXPECT(render_frame_no_stencil(s, canvas, draw_batchable_rects, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, 4, 4)[2] > 180);
        EXPECT(px_at(px, 60, 60)[0] > 180);
        EXPECT(px_at(px, W - 2, H - 2)[0] < 20);
    }

    /* A stencil-dependent fill in that strict pass is explicitly rejected;
     * it must not silently fall back to nonzero semantics or bind a stencil
     * pipeline incompatible with the attachment-free render pass. */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_add_rect(p, (flux_rect){16, 16, 64, 64});
        EXPECT(render_frame_no_stencil(s, canvas, draw_even_odd_path, p) ==
               FLUX_ERROR_INVALID_STATE);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, 32, 32)[0] < 20);
        flux_arena_reset(&arena);
    }

    /* --- linear gradient: red → blue across x --- */
    {
        EXPECT(render_frame(s, canvas, draw_linear_gradient, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *left = px_at(px, 2, H / 2);
        const uint8_t *mid = px_at(px, W / 2, H / 2);
        const uint8_t *right = px_at(px, W - 3, H / 2);
        EXPECT(left[0] > 220 && left[2] < 35);   /* red end */
        EXPECT(right[2] > 220 && right[0] < 35); /* blue end */
        /* Midpoint mixes both; monotone along the axis. */
        EXPECT(mid[0] > 64 && mid[0] < 200 && mid[2] > 64 && mid[2] < 200);
        EXPECT(left[0] > mid[0] && mid[0] > right[0]);
        EXPECT(right[2] > mid[2] && mid[2] > left[2]);
    }

    /* --- stroke: 8px horizontal line through the centre --- */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_move_to(p, 16.0f, H / 2.0f);
        flux_path_line_to(p, W - 16.0f, H / 2.0f);

        EXPECT(render_frame(s, canvas, draw_stroke, p) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* On the line: white. 8 px above it: background. Before the
         * butt cap at x=16: background. */
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        EXPECT(px_at(px, W / 2, H / 2 - 8)[0] < 20);
        EXPECT(px_at(px, W / 2, H / 2 + 8)[0] < 20);
        EXPECT(px_at(px, 8, H / 2)[0] < 20);
    }

    /* --- multi-subpath stroke: two separate lines in one path --- *
     * A move_to starts a new subpath; the stroker must render every
     * subpath, not just the first (regression: glyphs like a settings
     * gear or a sidebar with a divider lost all but their first
     * subpath). */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_move_to(p, 16.0f, H / 2.0f - 24.0f); /* subpath 1 */
        flux_path_line_to(p, W - 16.0f, H / 2.0f - 24.0f);
        flux_path_move_to(p, 16.0f, H / 2.0f + 24.0f); /* subpath 2 */
        flux_path_line_to(p, W - 16.0f, H / 2.0f + 24.0f);

        EXPECT(render_frame(s, canvas, draw_stroke, p) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Both lines must be white; the gap between them background. */
        EXPECT(px_at(px, W / 2, H / 2 - 24)[0] == 255); /* subpath 1 */
        EXPECT(px_at(px, W / 2, H / 2 + 24)[0] == 255); /* subpath 2 */
        EXPECT(px_at(px, W / 2, H / 2)[0] < 20);        /* gap */
    }

    /* --- donut fill: outer circle + reverse-wound inner hole --- */
    {
        const float cx = W / 2.0f, cy = H / 2.0f;
        const float r_out = 48.0f, r_in = 20.0f;

        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_add_circle(p, cx, cy, r_out);
        /* Inner contour wound opposite to flux_path_add_circle's
         * direction → a hole per ADR-0011. */
        flux_path_move_to(p, cx + r_in, cy);
        for (int i = 1; i < 32; ++i) {
            float a = -(float)i / 32.0f * 6.28318530718f;
            flux_path_line_to(p, cx + r_in * cosf(a), cy + r_in * sinf(a));
        }
        flux_path_close(p);

        EXPECT(render_frame(s, canvas, draw_donut, p) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Centre of the hole: background. Ring interior (between the
         * radii, four directions): white. Outside the outer circle:
         * background. */
        EXPECT(px_at(px, W / 2, H / 2)[0] < 20);
        uint32_t ring = (uint32_t)((r_in + r_out) / 2.0f);
        EXPECT(px_at(px, W / 2 + ring, H / 2)[0] == 255);
        EXPECT(px_at(px, W / 2 - ring, H / 2)[0] == 255);
        EXPECT(px_at(px, W / 2, H / 2 + ring)[0] == 255);
        EXPECT(px_at(px, W / 2, H / 2 - ring)[0] == 255);
        EXPECT(px_at(px, 2, 2)[0] < 20);
    }

    /* --- self-intersecting pentagram: stencil-then-cover (ADR-0014).
     * A 5-point star drawn edge-to-every-second-vertex self-intersects;
     * the ear clip stalls and previously produced an empty fill. Under
     * the nonzero winding rule the whole star — points AND the inner
     * pentagon (winding 2) — must fill. --- */
    {
        const float cx = W / 2.0f, cy = H / 2.0f, R = 52.0f;
        flux_point v[5];
        for (int i = 0; i < 5; ++i) {
            float a = (float)i * (6.28318530718f / 5.0f) - 1.57079632679f;
            v[i] = (flux_point){cx + R * cosf(a), cy + R * sinf(a)};
        }
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_move_to(p, v[0].x, v[0].y);
        for (int i = 1; i < 5; ++i) {
            int k = (i * 2) % 5; /* 0 → 2 → 4 → 1 → 3 → close */
            flux_path_line_to(p, v[k].x, v[k].y);
        }
        flux_path_close(p);

        EXPECT(render_frame(s, canvas, draw_donut, p) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Centre (inside the inner pentagon, winding count 2): filled. */
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        /* Inside the top point's spike (winding 1): filled. */
        uint32_t spike_y = (uint32_t)(cy - R) + 6;
        EXPECT(px_at(px, W / 2, spike_y)[0] == 255);
        /* Notch bisector between the top and upper-right spikes
         * (angle -54°, radius 0.7R; the boundary there is the inner
         * vertex at ~0.38R): outside the star body, background. */
        EXPECT(px_at(px, (uint32_t)(cx + 0.7f * R * 0.5878f),
                     (uint32_t)(cy - 0.7f * R * 0.8090f))[0] < 20);
        /* Far corner: background. */
        EXPECT(px_at(px, 2, 2)[0] < 20);

        /* The cover pass must zero the stencil it used: a SECOND fill
         * in a later frame must not be corrupted by leftover counts. */
        EXPECT(render_frame(s, canvas, draw_donut, p) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, W / 2, H / 2)[0] == 255);
        EXPECT(px_at(px, 2, 2)[0] < 20);
    }

    /* --- affine image transform + paint opacity --- */
    {
        /* A 2×2 quadrant image, rotated 90 degrees. Regression coverage for
         * per-vertex image UVs: the former screen-AABB UV reconstruction
         * sampled the wrong quadrant after rotation. A half-alpha white
         * paint simultaneously verifies image tint/opacity modulation. */
        uint32_t image_px[4] = {
            0xFF0000FFu, 0xFF00FF00u, /* red, green */
            0xFFFFFFFFu, 0xFFFF0000u, /* white, blue */
        };
        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.width = 2;
        idesc.height = 2;
        idesc.format = FLUX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = image_px;
        flux_image *image = nullptr;
        EXPECT(flux_image_create(d, &idesc, &image) == FLUX_OK);

        flux_sampler_desc sdesc = FLUX_SAMPLER_DESC_INIT;
        sdesc.min_filter = FLUX_FILTER_NEAREST;
        sdesc.mag_filter = FLUX_FILTER_NEAREST;
        sdesc.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
        flux_sampler *nearest = nullptr;
        EXPECT(flux_sampler_create(d, &sdesc, &nearest) == FLUX_OK);

        image_record_case tc = {
            .image =
                {
                    .image = image,
                    .sampler = nearest,
                    .paint = flux_paint_solid(flux_color_rgba_premul(255, 255, 255, 128)),
                },
            .record = FLUX_CANVAS_RECORD_INIT,
        };
        EXPECT(render_frame(s, canvas, draw_round_image, &tc.image) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, 64, 64)[0] > 80); /* image centre survives */
        EXPECT(px_at(px, 33, 33)[0] < 8);  /* square corner is clipped */

        EXPECT(render_frame(s, canvas, draw_image_through_independent_round_clip, &tc.image) ==
               FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(px_at(px, 64, 64)[0] > 40); /* destination content survives */
        EXPECT(px_at(px, 24, 64)[0] < 8);  /* inside dst, outside shared clip */
        EXPECT(px_at(px, 33, 33)[0] < 8);  /* shared clip has analytic corners */

        EXPECT(render_frame(s, canvas, record_rotated_image, &tc) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *top_right = px_at(px, 72, 48);    /* source top-left: red */
        const uint8_t *bottom_right = px_at(px, 72, 80); /* source top-right: green */
        const uint8_t *bottom_left = px_at(px, 56, 80);  /* source bottom-right: blue */
        const uint8_t *top_left = px_at(px, 56, 48);     /* source bottom-left: white */
        EXPECT(top_right[0] > 112 && top_right[0] < 144 && top_right[1] < 8 &&
               top_right[2] < 8);
        EXPECT(bottom_right[0] < 8 && bottom_right[1] > 112 && bottom_right[1] < 144 &&
               bottom_right[2] < 8);
        EXPECT(bottom_left[0] < 8 && bottom_left[1] < 8 && bottom_left[2] > 112 &&
               bottom_left[2] < 144);
        EXPECT(top_left[0] > 112 && top_left[0] < 144 && top_left[1] > 112 &&
               top_left[1] < 144 && top_left[2] > 112 && top_left[2] < 144);
        EXPECT(px_at(px, 40, 64)[0] < 8); /* outside the rotated 32×64 quad */

        /* A display-list segment owns both referenced bindless resources.
         * Drop the caller's image and custom-sampler references, then replay:
         * recycled handles would corrupt these pixels or trip validation. */
        uint8_t live[BYTES];
        memcpy(live, px, sizeof live);
        flux_sampler_release(nearest);
        flux_image_release(image);
        EXPECT(render_frame(s, canvas, replay_record, &tc.record) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        EXPECT(memcmp(live, px, sizeof live) == 0);
        flux_canvas_record_release(canvas, tc.record);
    }

    /* --- alpha-free image import ignores the undefined X channel --- */
    {
        const uint8_t xrgb_px[4] = {37, 113, 211, 0};
        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = FLUX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = xrgb_px;
        flux_image *image = nullptr;
        EXPECT(flux_image_create(d, &idesc, &image) == FLUX_OK);
        EXPECT(render_frame_no_stencil(s, canvas, draw_opaque_image, image) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(centre[0] == 37 && centre[1] == 113 && centre[2] == 211 && centre[3] == 255);
        flux_image_release(image);
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

        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.width = 8;
        idesc.height = 4;
        idesc.format = FLUX_FORMAT_R8_UNORM;
        idesc.initial_data = atlas_px;
        flux_image *atlas = nullptr;
        EXPECT(flux_image_create(d, &idesc, &atlas) == FLUX_OK);

        flux_sampler_desc sdesc = FLUX_SAMPLER_DESC_INIT;
        sdesc.min_filter = FLUX_FILTER_NEAREST;
        sdesc.mag_filter = FLUX_FILTER_NEAREST;
        sdesc.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
        flux_sampler *nearest = nullptr;
        EXPECT(flux_sampler_create(d, &sdesc, &nearest) == FLUX_OK);

        flux_glyph_quad quads[2] = {
            {.sx = 16,
             .sy = 16,
             .sw = 24,
             .sh = 24,
             .ax = 0,
             .ay = 0,
             .aw = 4,
             .ah = 4,
             .color = flux_color_rgba(255, 0, 0, 255)},
            {.sx = 72,
             .sy = 16,
             .sw = 24,
             .sh = 24,
             .ax = 4,
             .ay = 0,
             .aw = 4,
             .ah = 4,
             .color = flux_color_rgba(255, 255, 255, 255)},
        };
        flux_glyph_run_desc run = FLUX_GLYPH_RUN_DESC_INIT;
        run.atlas = atlas;
        run.sampler = nearest;
        run.quads = quads;
        run.quad_count = 2;

        EXPECT(render_frame(s, canvas, draw_glyph_run, &run) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Quad A centre: full coverage × red tint = pure red. */
        const uint8_t *a = px_at(px, 28, 28);
        EXPECT(a[0] > 240 && a[1] < 12 && a[2] < 12);

        /* Quad B centre: quarter coverage × white = ~64 grey
         * (premultiplied over black background). */
        const uint8_t *b = px_at(px, 84, 28);
        EXPECT(b[0] > 48 && b[0] < 84);
        EXPECT(b[1] > 48 && b[1] < 84);

        /* Between the quads and outside them: background. */
        EXPECT(px_at(px, 56, 28)[0] < 20);
        EXPECT(px_at(px, 28, 60)[0] < 20);

        flux_sampler_release(nearest);
        flux_image_release(atlas);
    }

#if defined(FLUX_TEXT_HAVE_FTHB)
    /* --- public text draw honours every family slot.
     * Regression: txt_ensure_face_loaded accepted only the two weight
     * slots for sans-serif, so serif/mono shaped to zero glyphs. wisp pins
     * fenced code blocks to MONO, which made code backgrounds render while
     * the actual code vanished. --- */
    {
        flux_text *text = nullptr;
        flux_text_desc td = {.device = d, .scale = 1.0f};
        EXPECT(flux_text_create(&td, &text) == FLUX_OK);
        EXPECT(text != nullptr);

        text_family_case cases[3] = {
            {text, &arena, FLUX_TEXT_FAMILY_SANS, 12.0f},
            {text, &arena, FLUX_TEXT_FAMILY_SERIF, 50.0f},
            {text, &arena, FLUX_TEXT_FAMILY_MONO, 88.0f},
        };

        for (int i = 0; i < 3; i++) {
            const char *text_s = "Visible";
            flux_text_style style = {
                .size_px = 24.0f,
                .weight = 400.0f,
                .color = flux_color_rgba(255, 255, 255, 255),
                .family = cases[i].family,
            };
            flux_text_metrics m = flux_text_measure(text, text_s, strlen(text_s), &style);
            EXPECT(m.width > 0.0f);
            EXPECT(m.height > 0.0f);

            EXPECT(render_frame(s, canvas, draw_text_family, &cases[i]) == FLUX_OK);
            memset(px, 0xCD, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
            EXPECT(region_has_ink(px, 10, (uint32_t)cases[i].y, 120, (uint32_t)cases[i].y + 32));
        }

        flux_text_destroy(text);
    }
#endif

    flux_arena_destroy(&arena);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
