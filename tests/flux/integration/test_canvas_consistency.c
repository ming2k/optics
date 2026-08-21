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
#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

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

static void draw_scene(flux_canvas *canvas, flux_arena *arena) {
    /* Linear gradient over the top half. */
    flux_gradient_stop stops[3] = {
        {0.0f, flux_color_rgba(230, 30, 60, 255)},
        {0.5f, flux_color_rgba(30, 200, 90, 255)},
        {1.0f, flux_color_rgba(40, 60, 220, 255)},
    };
    flux_paint grad =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 3);
    flux_canvas_fill_rect(canvas, (flux_rect){0, 0, (float)W, (float)H / 2}, &grad);

    /* Radial gradient disc bottom-left. */
    flux_paint radial = flux_paint_radial_gradient((flux_point){32, 96}, 24.0f, stops, 2);
    flux_canvas_fill_rect(canvas, (flux_rect){8, 72, 48, 48}, &radial);

    /* Solid rounded rect (SDF path) bottom-right. */
    flux_canvas_fill_rrect(canvas, (flux_rect){72, 72, 48, 40}, 10.0f,
                           flux_color_rgba(240, 220, 40, 255));

    /* Stroked rounded rect overlapping both. */
    flux_canvas_stroke_rrect(canvas, (flux_rect){40, 64, 48, 48}, 8.0f,
                             flux_color_rgba(255, 255, 255, 255), 3.0f);

    /* Checkerboard strip of solid rects (vertex-colour batching path). */
    for (uint32_t i = 0; i < 16; ++i) {
        flux_color c =
            (i & 1u) ? flux_color_rgba(20, 20, 30, 255) : flux_color_rgba(200, 200, 210, 255);
        flux_canvas_fill_rect_color(canvas, (flux_rect){(float)(i * 8), 52, 8, 8}, c);
    }

    /* Even-odd donut (tessellator + hole bridging). */
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) == FLUX_OK) {
        flux_path_add_circle(p, 104, 24, 14.0f);
        flux_path_add_circle(p, 104, 24, 6.0f);
        flux_paint paint = flux_paint_solid(flux_color_rgba(120, 220, 255, 255));
        paint.fill_rule = FLUX_FILL_EVEN_ODD;
        flux_canvas_fill_path(canvas, p, &paint);
    }
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
    flux_canvas *cpu = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &cpu) == FLUX_OK);
    EXPECT(cpu != nullptr);

    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 64 * 1024, nullptr) == FLUX_OK);

    flux_color clear = flux_color_rgba(12, 12, 16, 255);
    EXPECT(flux_canvas_cpu_begin(cpu, &clear) == FLUX_OK);
    draw_scene(cpu, &arena);
    flux_canvas_cpu_end(cpu);

    uint32_t cpu_w = 0, cpu_h = 0, cpu_stride = 0;
    const uint8_t *cpu_px = flux_canvas_cpu_pixels(cpu, &cpu_w, &cpu_h, &cpu_stride);
    EXPECT(cpu_px != nullptr && cpu_w == W && cpu_h == H);

    /* ---- GPU ---- */
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_canvas_consistency: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    }
    flux_canvas *gpu = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        EXPECT(flux_canvas_create(&cd, &gpu) == FLUX_OK);
    }

    static uint8_t gpu_px[BYTES];
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        EXPECT(flux_canvas_begin(gpu, frame, &clear) == FLUX_OK);
        draw_scene(gpu, &arena);
        flux_canvas_end(gpu);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, gpu_px, BYTES) == FLUX_OK);
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
            EXPECT(diff <= PROBE_TOL);
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
    EXPECT(mean <= MEAN_TOL);
    EXPECT(edge_frac <= EDGE_FRAC_MAX);
    if (mean > MEAN_TOL || edge_frac > EDGE_FRAC_MAX)
        fprintf(stderr, "  mean|diff|=%.4f edge_frac=%.4f (edges=%u)\n", mean, edge_frac,
                edge_pixels);

    flux_canvas_destroy(gpu);
    flux_surface_release(s);
    flux_device_release(d);
    flux_canvas_destroy(cpu);
    flux_arena_destroy(&arena);
    TEST_SUMMARY();
}
