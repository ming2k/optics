/*
 * Coverage ceiling (ADR-0119 S3 target).
 *
 * This test exists to make S3's motivation a *measurement* rather than a claim
 * in an ADR. It draws the same oblique edge twice — once through the GPU path,
 * which anti-aliases with 4× MSAA, and once through the CPU coverage path — and
 * counts the distinct coverage values along that edge.
 *
 * The numbers are structural, not incidental:
 *   - a sample count can only express (samples + 1) coverage levels, so 4× MSAA
 *     yields five, for every edge, on every device;
 *   - analytic coverage is continuous, so the count grows with the number of
 *     distinct sub-pixel phases the edge crosses.
 *
 * So this asserts the *gap* (GPU materially poorer than CPU) and pins the
 * ceiling from above. When S3 gives the device a coverage backend, the GPU
 * assertion is the one that must be inverted — and this test will fail loudly if
 * someone forgets, which is the point of writing it now.
 */
#include "test_helpers.h"

#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>
#include <flux/flux.h>

#include <string.h>

#define W 128u
#define H 128u
#define MAX_LEVELS 64

/* A triangle whose slanted edge crosses many sub-pixel phases: a 45° edge would
 * land corner-to-corner on every pixel and legitimately produce only three
 * values, which would measure the geometry rather than the rasteriser. */
static void draw_oblique(flux_canvas *c, flux_arena *arena) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    flux_path_move_to(p, 0.0f, 6.0f);
    flux_path_line_to(p, 127.0f, 21.0f);
    flux_path_line_to(p, 127.0f, 127.0f);
    flux_path_line_to(p, 0.0f, 127.0f);
    flux_path_close(p);
    flux_brush brush = flux_brush_solid(flux_color_rgba(255, 255, 255, 255));
    flux_canvas_fill_path(c, p, FLUX_FILL_NON_ZERO, &brush);
}

/* Distinct green values in the vertical band that contains the slanted edge. */
static int count_levels(const uint8_t *px, uint32_t stride, uint32_t w, uint32_t h) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    for (uint32_t y = 8; y < 30 && y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            seen[px[(size_t)y * stride + x * 4 + 1]] = true; /* green */
    int n = 0;
    for (int i = 0; i < 256; ++i)
        n += seen[i];
    return n;
}

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 64 * 1024, nullptr) == FLUX_OK);

    /* ---- CPU: analytic coverage ---- */
    int cpu_levels = 0;
    {
        flux_canvas *cpu = nullptr;
        EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &cpu) == FLUX_OK);
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(cpu, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .clear_color = &clear}) == FLUX_OK);
        draw_oblique(cpu, &arena);
        EXPECT(flux_canvas_end(cpu) == FLUX_OK);
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *px = flux_canvas_cpu_pixels(cpu, &w, &h, &stride);
        EXPECT(px != nullptr);
        if (px)
            cpu_levels = count_levels(px, stride, w, h);
        flux_canvas_release(cpu);
    }
    fprintf(stderr, "  CPU solid fill:        %d distinct levels along the edge\n", cpu_levels);
    /* INV-COV-02: continuous coverage, far beyond what a sample count can carry. */
    EXPECT(cpu_levels > 16);

    /* The coverage seam answers WHERE a pixel is painted, so it is not specific
     * to solid paint: a gradient fill must reach it too, and therefore carry the
     * same continuous coverage. Measured the same way on the same geometry. */
    int grad_levels = 0;
    {
        flux_canvas *cpu = nullptr;
        EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &cpu) == FLUX_OK);
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(cpu, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .clear_color = &clear}) == FLUX_OK);
        flux_path *p = nullptr;
        if (flux_path_create(&p, &arena) == FLUX_OK) {
            flux_path_move_to(p, 0.0f, 6.0f);
            flux_path_line_to(p, 127.0f, 21.0f);
            flux_path_line_to(p, 127.0f, 127.0f);
            flux_path_line_to(p, 0.0f, 127.0f);
            flux_path_close(p);
            flux_color a = flux_color_rgba(0, 200, 40, 255);
            flux_color b = flux_color_rgba(240, 240, 240, 255);
            flux_gradient_stop stops[2] = {{0.0f, a}, {1.0f, b}};
            flux_brush grad =
                flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){0, 127}, stops, 2);
            flux_canvas_fill_path(cpu, p, FLUX_FILL_NON_ZERO, &grad);
        }
        EXPECT(flux_canvas_end(cpu) == FLUX_OK);
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *px = flux_canvas_cpu_pixels(cpu, &w, &h, &stride);
        EXPECT(px != nullptr);
        if (px)
            grad_levels = count_levels(px, stride, w, h);
        flux_canvas_release(cpu);
    }
    fprintf(stderr, "  CPU gradient fill:     %d distinct levels along the edge\n", grad_levels);
    EXPECT(grad_levels > 16);

    /* ---- GPU: 4x MSAA ---- */
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "  no Vulkan device; GPU half skipped\n");
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
    static uint8_t gpu_px[W * H * 4];
    {
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        EXPECT(flux_canvas_begin(gpu, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &clear}) == FLUX_OK);
        draw_oblique(gpu, &arena);
        EXPECT(flux_canvas_end(gpu) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, gpu_px, sizeof(gpu_px)) == FLUX_OK);
    }
    int gpu_levels = count_levels(gpu_px, W * 4, W, H);
    fprintf(stderr, "  GPU coverage backend:  %d distinct levels along the edge\n", gpu_levels);

    /* CONTRACT SATISFIED: ADR-0116/ADR-0119 S3 / ADR-0120 completed.
     * The device path now rasterizes with continuous analytic coverage (> 16 levels, INV-COV-02).
     * The legacy 4x MSAA 16-level ceiling is inverted and closed. */
    EXPECT(gpu_levels > 16);
    EXPECT(gpu_levels >= 80);

    flux_canvas_release(gpu);
    flux_surface_release(s);
    flux_device_release(d);
    flux_arena_deinit(&arena);
    TEST_SUMMARY();
}
