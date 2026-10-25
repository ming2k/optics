/*
 * Tile-compute fine rasterizer (ADR-0120 Phase B4 gate).
 *
 * Assert that per-pixel area integration in canvas_tile_rasterize matches the CPU
 * reference rasterizer (coverage_raster.c) to float precision on the S0 corpus,
 * and carries continuous coverage > 16 distinct levels (INV-COV-02).
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/flux.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u

void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

static void compare_tile_vs_cpu(const char *label, const flux_point *pts,
                                const flux_canvas_contour *cons, uint32_t ncons,
                                flux_fill_rule rule, float ox, float oy, int min_levels) {
    float *cpu_cov = malloc((size_t)W * H * sizeof(float));
    float *tile_cov = malloc((size_t)W * H * sizeof(float));
    EXPECT(cpu_cov && tile_cov);
    if (!cpu_cov || !tile_cov) {
        free(cpu_cov);
        free(tile_cov);
        return;
    }

    /* 1. CPU reference rasterizer */
    EXPECT(canvas_coverage_rasterize(pts, cons, ncons, rule, ox, oy, W, H, cpu_cov) == FLUX_OK);

    /* 2. Tile-compute rasterizer */
    flux_tile_segment segs[4096];
    flux_rect bounds;
    uint32_t n_segs = canvas_tile_extract_segments(pts, cons, ncons, segs, 4096, &bounds);
    EXPECT(n_segs > 0);
    EXPECT(canvas_tile_rasterize(segs, n_segs, rule, ox, oy, W, H, tile_cov) == FLUX_OK);

    /* 3. Pixel-for-pixel comparison */
    double max_diff = 0.0;
    double sum_diff = 0.0;
    bool seen_levels[256];
    memset(seen_levels, 0, sizeof(seen_levels));

    for (size_t i = 0; i < (size_t)W * H; ++i) {
        double diff = fabs((double)tile_cov[i] - (double)cpu_cov[i]);
        if (diff > max_diff)
            max_diff = diff;
        sum_diff += diff;

        int byte_val = (int)floor(tile_cov[i] * 255.0f + 0.5f);
        if (byte_val >= 0 && byte_val <= 255)
            seen_levels[byte_val] = true;
    }

    double mean_diff = sum_diff / (double)(W * H);
    int distinct_levels = 0;
    for (int k = 0; k < 256; ++k)
        distinct_levels += seen_levels[k];

    fprintf(stderr, "  %-24s max_diff=%.7f mean_diff=%.7f levels=%d\n", label, max_diff, mean_diff,
            distinct_levels);

    int printed = 0;
    for (uint32_t y = 0; y < H && printed < 5; ++y) {
        for (uint32_t x = 0; x < W && printed < 5; ++x) {
            size_t idx = (size_t)y * W + x;
            double d = fabs((double)tile_cov[idx] - (double)cpu_cov[idx]);
            if (d > 0.5) {
                fprintf(stderr, "    diff at (%u, %u): tile=%.4f cpu=%.4f (diff=%.4f)\n", x, y,
                        tile_cov[idx], cpu_cov[idx], d);
                printed++;
            }
        }
    }

    /* Gate B4 tolerances: agreement to float precision */
    EXPECT(max_diff <= 1e-4);
    EXPECT(mean_diff <= 1e-5);

    if (min_levels > 0)
        EXPECT(distinct_levels >= min_levels);

    free(cpu_cov);
    free(tile_cov);
}

int main(void) {
    /* 1. Square at fractional origin */
    {
        flux_point sq[4] = {{-20, -20}, {20, -20}, {20, 20}, {-20, 20}};
        flux_canvas_contour con = {.start = 0, .count = 4, .closed = true};
        compare_tile_vs_cpu("square fractional", sq, &con, 1, FLUX_FILL_NON_ZERO, -64.37f, -64.81f,
                            0);
    }

    /* 2. 64-gon Circle */
    {
        const uint32_t N = 64;
        flux_point circ[64];
        for (uint32_t i = 0; i < N; ++i) {
            double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
            circ[i] = (flux_point){(float)(35.0 * cos(a)), (float)(35.0 * sin(a))};
        }
        flux_canvas_contour con = {.start = 0, .count = N, .closed = true};
        compare_tile_vs_cpu("circle 64-gon", circ, &con, 1, FLUX_FILL_NON_ZERO, -64.12f, -64.55f,
                            16);
    }

    /* 3. Donut (hole) */
    {
        flux_point donut[8] = {
            {-30, -30}, {30, -30}, {30, 30}, {-30, 30}, {-15, -15}, {15, -15}, {15, 15}, {-15, 15},
        };
        flux_canvas_contour cons[2] = {
            {.start = 0, .count = 4, .closed = true},
            {.start = 4, .count = 4, .closed = true},
        };
        compare_tile_vs_cpu("donut hole (even-odd)", donut, cons, 2, FLUX_FILL_EVEN_ODD, -64.0f,
                            -64.0f, 0);
    }

    /* 4. High-contrast oblique edge: INV-COV-02 ceiling test probe */
    {
        flux_point oblique[4] = {
            {0.0f, 6.0f},
            {127.0f, 21.0f},
            {127.0f, 127.0f},
            {0.0f, 127.0f},
        };
        flux_canvas_contour con = {.start = 0, .count = 4, .closed = true};
        compare_tile_vs_cpu("oblique edge (INV-COV-02)", oblique, &con, 1, FLUX_FILL_NON_ZERO, 0.0f,
                            0.0f, 17);
    }

    TEST_SUMMARY();
}
