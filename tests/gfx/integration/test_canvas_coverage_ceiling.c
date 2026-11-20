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
#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>

#include <string.h>

#define W 128u
#define H 128u
#define MAX_LEVELS 64

/* A triangle whose slanted edge crosses many sub-pixel phases: a 45° edge would
 * land corner-to-corner on every pixel and legitimately produce only three
 * values, which would measure the geometry rather than the rasteriser. */
static void draw_oblique(optics_gfx_canvas *c, optics_gfx_arena *arena) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    optics_gfx_path_move_to(p, 0.0f, 6.0f);
    optics_gfx_path_line_to(p, 127.0f, 21.0f);
    optics_gfx_path_line_to(p, 127.0f, 127.0f);
    optics_gfx_path_line_to(p, 0.0f, 127.0f);
    optics_gfx_path_close(p);
    optics_gfx_brush brush = optics_gfx_brush_solid(optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_fill_path(c, p, OPTICS_GFX_FILL_NON_ZERO, &brush);
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
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 64 * 1024, nullptr) == OPTICS_GFX_OK);

    /* ---- CPU: analytic coverage ---- */
    int cpu_levels = 0;
    {
        optics_gfx_canvas *cpu = nullptr;
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = W,
                                                .height = H},
                      &cpu) == OPTICS_GFX_OK);
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      cpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
        draw_oblique(cpu, &arena);
        OT_EXPECT(optics_gfx_canvas_end(cpu) == OPTICS_GFX_OK);
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *px = optics_gfx_canvas_cpu_pixels(cpu, &w, &h, &stride);
        OT_EXPECT(px != nullptr);
        if (px)
            cpu_levels = count_levels(px, stride, w, h);
        optics_gfx_canvas_release(cpu);
    }
    fprintf(stderr, "  CPU solid fill:        %d distinct levels along the edge\n", cpu_levels);
    /* INV-COV-02: continuous coverage, far beyond what a sample count can carry. */
    OT_EXPECT(cpu_levels > 16);

    /* The coverage seam answers WHERE a pixel is painted, so it is not specific
     * to solid paint: a gradient fill must reach it too, and therefore carry the
     * same continuous coverage. Measured the same way on the same geometry. */
    int grad_levels = 0;
    {
        optics_gfx_canvas *cpu = nullptr;
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = W,
                                                .height = H},
                      &cpu) == OPTICS_GFX_OK);
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      cpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
        optics_gfx_path *p = nullptr;
        if (optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK) {
            optics_gfx_path_move_to(p, 0.0f, 6.0f);
            optics_gfx_path_line_to(p, 127.0f, 21.0f);
            optics_gfx_path_line_to(p, 127.0f, 127.0f);
            optics_gfx_path_line_to(p, 0.0f, 127.0f);
            optics_gfx_path_close(p);
            optics_gfx_color a = optics_gfx_color_rgba(0, 200, 40, 255);
            optics_gfx_color b = optics_gfx_color_rgba(240, 240, 240, 255);
            optics_gfx_gradient_stop stops[2] = {{0.0f, a}, {1.0f, b}};
            optics_gfx_brush grad = optics_gfx_brush_linear_gradient(
                (optics_gfx_point){0, 0}, (optics_gfx_point){0, 127}, stops, 2);
            optics_gfx_canvas_fill_path(cpu, p, OPTICS_GFX_FILL_NON_ZERO, &grad);
        }
        OT_EXPECT(optics_gfx_canvas_end(cpu) == OPTICS_GFX_OK);
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *px = optics_gfx_canvas_cpu_pixels(cpu, &w, &h, &stride);
        OT_EXPECT(px != nullptr);
        if (px)
            grad_levels = count_levels(px, stride, w, h);
        optics_gfx_canvas_release(cpu);
    }
    fprintf(stderr, "  CPU gradient fill:     %d distinct levels along the edge\n", grad_levels);
    OT_EXPECT(grad_levels > 16);

    /* ---- GPU: 4x MSAA ---- */
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "  no Vulkan device; GPU half skipped\n");
        OT_SKIP("no Vulkan device (GPU half)");
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
    static uint8_t gpu_px[W * H * 4];
    {
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(
                      gpu, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
        draw_oblique(gpu, &arena);
        OT_EXPECT(optics_gfx_canvas_end(gpu) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, gpu_px, sizeof(gpu_px)) == OPTICS_GFX_OK);
    }
    int gpu_levels = count_levels(gpu_px, W * 4, W, H);
    fprintf(stderr, "  GPU coverage backend:  %d distinct levels along the edge\n", gpu_levels);

    /* CONTRACT SATISFIED: ADR-0116/ADR-0119 S3 / ADR-0120 completed.
     * The device path now rasterizes with continuous analytic coverage (> 16 levels, INV-COV-02).
     * The legacy 4x MSAA 16-level ceiling is inverted and closed. */
    OT_EXPECT(gpu_levels > 16);
    OT_EXPECT(gpu_levels >= 80);

    optics_gfx_canvas_release(gpu);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
