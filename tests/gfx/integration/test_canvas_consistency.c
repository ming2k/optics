/*
 * Cross-platform rendering consistency: one fixed canvas scene rendered
 * by the GPU backend (offscreen surface, ADR-0013) and by the software
 * CPU backend (ADR-0019), then compared pixel-by-pixel.
 *
 * The CPU backend is the rendering oracle: it is a pure-C, bit-exact
 * reimplementation of the canvas pipeline, so it produces the same
 * output on every OS. A platform port is consistent when its GPU path
 * agrees with that oracle within tolerance — this is the test the
 * Windows/macOS CI lanes run to prove it.
 *
 * MSAA (GPU, 4x) and supersampling (CPU, 4x) antialiasing legitimately
 * differ at coverage edges, so the assertions combine tight probes at
 * interior points with global statistics that bound the edge deviation:
 *   - probe points (flat interiors, gradient stops) must match closely;
 *   - the mean absolute channel difference over the frame stays small;
 *   - only a small fraction of pixels may show large (edge) deviation.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u
#define BYTES (W * H * 4u)

/* Tolerances (8-bit channel units). */
#define PROBE_TOL 4        /* interior probes: |gpu - cpu| per channel  */
#define MEAN_TOL 2.5       /* mean |diff| over the whole frame          */
#define EDGE_TOL 12        /* per-pixel "edge deviation" threshold      */
#define EDGE_FRAC_MAX 0.03 /* allowed fraction of edge-deviating pixels */

static void draw_scene(optics_gfx_canvas *canvas, optics_gfx_arena *arena) {
    /* Linear gradient over the top half. */
    optics_gfx_gradient_stop stops[3] = {
        {0.0f, optics_gfx_color_rgba(230, 30, 60, 255)},
        {0.5f, optics_gfx_color_rgba(30, 200, 90, 255)},
        {1.0f, optics_gfx_color_rgba(40, 60, 220, 255)},
    };
    optics_gfx_brush grad = optics_gfx_brush_linear_gradient(
        (optics_gfx_point){0, 0}, (optics_gfx_point){(float)W, 0}, stops, 3);
    optics_gfx_canvas_fill_rect(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H / 2}, &grad);

    /* Radial gradient disc bottom-left. */
    optics_gfx_brush radial =
        optics_gfx_brush_radial_gradient((optics_gfx_point){32, 96}, 24.0f, stops, 2);
    optics_gfx_canvas_fill_rect(canvas, (optics_gfx_rect){8, 72, 48, 48}, &radial);

    /* Solid rounded rect (SDF path) bottom-right. */
    optics_gfx_canvas_fill_rrect(canvas, (optics_gfx_rect){72, 72, 48, 40}, 10.0f,
                                 optics_gfx_color_rgba(240, 220, 40, 255));

    /* Stroked rounded rect overlapping both. */
    optics_gfx_canvas_stroke_rrect(canvas, (optics_gfx_rect){40, 64, 48, 48}, 8.0f,
                                   optics_gfx_color_rgba(255, 255, 255, 255), 3.0f);

    /* Checkerboard strip of solid rects (vertex-colour batching path). */
    for (uint32_t i = 0; i < 16; ++i) {
        optics_gfx_color c = (i & 1u) ? optics_gfx_color_rgba(20, 20, 30, 255)
                                      : optics_gfx_color_rgba(200, 200, 210, 255);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){(float)(i * 8), 52, 8, 8}, c);
    }

    /* Even-odd donut (tessellator + hole bridging). */
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) == OPTICS_GFX_OK) {
        optics_gfx_path_add_circle(p, 104, 24, 14.0f);
        optics_gfx_path_add_circle(p, 104, 24, 6.0f);
        optics_gfx_brush paint = optics_gfx_brush_solid(optics_gfx_color_rgba(120, 220, 255, 255));
        optics_gfx_fill_rule paint_rule = OPTICS_GFX_FILL_EVEN_ODD;
        optics_gfx_canvas_fill_path(canvas, p, paint_rule, &paint);
    }

    /* Rotated analytic shapes (ADR-0118): the SDF pipeline evaluates its field
     * in shape space, so a rotated rounded rect / squircle must match the CPU
     * oracle's evaluation of the same field rather than being drawn
     * axis-aligned. Placed low-left, away from the probe points, and compared
     * through the scene-wide statistics. */
    optics_gfx_canvas_save(canvas);
    optics_gfx_canvas_translate(canvas,
                                &(optics_gfx_canvas_translate_opts){.x = 28.0f, .y = 104.0f});
    optics_gfx_canvas_rotate(canvas, 0.5235988f); /* 30 degrees */
    optics_gfx_canvas_translate(canvas,
                                &(optics_gfx_canvas_translate_opts){.x = -28.0f, .y = -104.0f});
    optics_gfx_canvas_fill_rrect(canvas, (optics_gfx_rect){12, 88, 32, 32}, 9.0f,
                                 optics_gfx_color_rgba(250, 160, 60, 255));
    optics_gfx_geometry sq =
        optics_gfx_geom_squircle((optics_gfx_rect){48, 92, 30, 26}, 11.0f, 0.85f);
    optics_gfx_brush sq_brush = optics_gfx_brush_solid(optics_gfx_color_rgba(90, 240, 160, 255));
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &sq, .brush = &sq_brush});
    optics_gfx_canvas_restore(canvas);
}

/* Tight per-channel probes at flat interiors / exact gradient stops. */
typedef struct probe {
    uint32_t x, y;
    uint8_t r, g, b, a; /* expected from the CPU oracle, filled at runtime */
} probe;

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

int main(void) {
    /* ---- CPU oracle ---- */
    optics_gfx_canvas *cpu = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &cpu) == OPTICS_GFX_OK);
    OT_EXPECT(cpu != nullptr);

    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 64 * 1024, nullptr) == OPTICS_GFX_OK);

    optics_gfx_color clear = optics_gfx_color_rgba(12, 12, 16, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  cpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == OPTICS_GFX_OK);
    draw_scene(cpu, &arena);
    OT_EXPECT(optics_gfx_canvas_end(cpu) == OPTICS_GFX_OK);

    uint32_t cpu_w = 0, cpu_h = 0, cpu_stride = 0;
    const uint8_t *cpu_px = optics_gfx_canvas_cpu_pixels(cpu, &cpu_w, &cpu_h, &cpu_stride);
    OT_EXPECT(cpu_px != nullptr && cpu_w == W && cpu_h == H);

    /* ---- GPU ---- */
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_consistency: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    }
    optics_gfx_canvas *gpu = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &gpu) == OPTICS_GFX_OK);
    }

    static uint8_t gpu_px[BYTES];
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(
                      gpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
        draw_scene(gpu, &arena);
        OT_EXPECT(optics_gfx_canvas_end(gpu) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, gpu_px, BYTES) == OPTICS_GFX_OK);
    }

    /* ---- Interior probes (CPU value is the reference) ---- */
    static const struct {
        uint32_t x, y;
    } probes[] = {
        {4, 32},    /* gradient near left stop   */
        {64, 32},   /* gradient mid-stop         */
        {124, 32},  /* gradient near right stop  */
        {96, 92},   /* solid rrect interior      */
        {10, 54},   /* checker dark cell         */
        {18, 54},   /* checker light cell        */
        {104, 24},  /* donut hole (clear colour) */
        {104, 12},  /* donut ring top            */
        {120, 120}, /* clear corner              */
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        const uint8_t *ref = cpu_px + probes[i].y * cpu_stride + probes[i].x * 4;
        const uint8_t *got = px_at(gpu_px, probes[i].x, probes[i].y);
        for (int ch = 0; ch < 4; ++ch) {
            int diff = abs((int)got[ch] - (int)ref[ch]);
            OT_EXPECT(diff <= PROBE_TOL);
            if (diff > PROBE_TOL)
                fprintf(stderr, "  probe (%u,%u) ch%d: gpu=%u cpu=%u\n", probes[i].x, probes[i].y,
                        ch, got[ch], ref[ch]);
        }
    }

    /* ---- Global statistics: bound the AA-edge deviation ---- */
    double sum = 0.0;
    uint32_t edge_pixels = 0;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *ref = cpu_px + y * cpu_stride + x * 4;
            const uint8_t *got = px_at(gpu_px, x, y);
            uint32_t max_ch = 0;
            for (int ch = 0; ch < 4; ++ch) {
                uint32_t dch = (uint32_t)abs((int)got[ch] - (int)ref[ch]);
                sum += (double)dch;
                if (dch > max_ch)
                    max_ch = dch;
            }
            if (max_ch > EDGE_TOL)
                edge_pixels++;
        }
    }
    double mean = sum / (double)(W * H * 4u);
    double edge_frac = (double)edge_pixels / (double)(W * H);
    OT_EXPECT(mean <= MEAN_TOL);
    OT_EXPECT(edge_frac <= EDGE_FRAC_MAX);
    if (mean > MEAN_TOL || edge_frac > EDGE_FRAC_MAX)
        fprintf(stderr, "  mean|diff|=%.4f edge_frac=%.4f (edges=%u)\n", mean, edge_frac,
                edge_pixels);

    optics_gfx_canvas_release(gpu);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    optics_gfx_canvas_release(cpu);
    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
