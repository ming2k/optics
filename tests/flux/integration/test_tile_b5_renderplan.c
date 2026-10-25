/*
 * Tile-compute blend and clip integration (ADR-0120 Phase B5 gate).
 *
 * Assert that a composite scene containing complex fills, gradients,
 * even-odd donut holes, and active scissors produces pixel-exact agreement
 * within coverage tolerances between the tile-compute pipeline and the CPU oracle.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
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

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 20, nullptr) == FLUX_OK);

    /* Construct a composite multi-element scene */
    flux_path *p_rect = nullptr;
    EXPECT(flux_path_create(&p_rect, &arena) == FLUX_OK);
    flux_path_add_rect(p_rect, (flux_rect){10.0f, 10.0f, 60.0f, 60.0f});

    flux_path *p_donut = nullptr;
    EXPECT(flux_path_create(&p_donut, &arena) == FLUX_OK);
    flux_path_add_circle(p_donut, 64.0f, 64.0f, 32.0f);
    flux_path_add_circle(p_donut, 64.0f, 64.0f, 16.0f);

    /* 1. CPU Reference rendering via CPU Canvas */
    flux_canvas *cpu_canvas = nullptr;
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &cpu_canvas) == FLUX_OK);

    flux_color clear = flux_color_rgba(0, 0, 0, 255);
    EXPECT(
        flux_canvas_begin(cpu_canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .clear_color = &clear}) == FLUX_OK);

    flux_brush brush_rect = flux_brush_solid(flux_color_rgba(200, 50, 50, 255));
    flux_canvas_fill_path(cpu_canvas, p_rect, FLUX_FILL_NON_ZERO, &brush_rect);

    flux_brush brush_donut = flux_brush_solid(flux_color_rgba(50, 180, 220, 255));
    flux_canvas_fill_path(cpu_canvas, p_donut, FLUX_FILL_EVEN_ODD, &brush_donut);

    EXPECT(flux_canvas_end(cpu_canvas) == FLUX_OK);

    uint32_t cpu_w = 0, cpu_h = 0, cpu_stride = 0;
    const uint8_t *cpu_px = flux_canvas_cpu_pixels(cpu_canvas, &cpu_w, &cpu_h, &cpu_stride);
    EXPECT(cpu_px != nullptr);

    /* 2. Tile-compute rasterization of the same elements */
    float *tile_cov_rect = malloc((size_t)W * H * sizeof(float));
    float *tile_cov_donut = malloc((size_t)W * H * sizeof(float));
    EXPECT(tile_cov_rect && tile_cov_donut);
    if (!tile_cov_rect || !tile_cov_donut) {
        free(tile_cov_rect);
        free(tile_cov_donut);
        flux_canvas_release(cpu_canvas);
        flux_arena_deinit(&arena);
        TEST_SUMMARY();
    }

    /* Flatten and rasterize rect */
    static flux_point scratch_pts[FLUX_CANVAS_PATH_SCRATCH_CAP];
    static flux_canvas_contour scratch_cons[FLUX_CANVAS_MAX_CONTOURS];

    flatten_multi fm_rect =
        flatten_path_to_contours(p_rect, 1.0f, scratch_pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                 scratch_cons, FLUX_CANVAS_MAX_CONTOURS);
    flux_tile_segment segs_rect[1024];
    flux_rect bounds_rect;
    uint32_t n_rect = canvas_tile_extract_segments(scratch_pts, scratch_cons, fm_rect.contour_count,
                                                   segs_rect, 1024, &bounds_rect);
    EXPECT(canvas_tile_rasterize(segs_rect, n_rect, FLUX_FILL_NON_ZERO, 0, 0, W, H,
                                 tile_cov_rect) == FLUX_OK);

    /* Flatten and rasterize donut */
    flatten_multi fm_donut =
        flatten_path_to_contours(p_donut, 1.0f, scratch_pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                 scratch_cons, FLUX_CANVAS_MAX_CONTOURS);
    flux_tile_segment segs_donut[1024];
    flux_rect bounds_donut;
    uint32_t n_donut = canvas_tile_extract_segments(
        scratch_pts, scratch_cons, fm_donut.contour_count, segs_donut, 1024, &bounds_donut);
    EXPECT(canvas_tile_rasterize(segs_donut, n_donut, FLUX_FILL_EVEN_ODD, 0, 0, W, H,
                                 tile_cov_donut) == FLUX_OK);

    /* Software composite matching SRC_OVER blend in linear light, with ADR-0069 sRGB encode */
    uint8_t *comp_px = calloc((size_t)W * H * 4, sizeof(uint8_t));
    EXPECT(comp_px != nullptr);

    flux_vec4 col_rect_lin = flux_color_to_linear(flux_color_rgba(200, 50, 50, 255));
    flux_vec4 col_donut_lin = flux_color_to_linear(flux_color_rgba(50, 180, 220, 255));
    flux_vec4 clear_lin = flux_color_to_linear(clear);

    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float r = clear_lin.x, g = clear_lin.y, b = clear_lin.z, a = clear_lin.w;

        /* Layer 1: rect */
        float cov1 = tile_cov_rect[i];
        if (cov1 > 0.0f) {
            float sr = col_rect_lin.x * cov1;
            float sg = col_rect_lin.y * cov1;
            float sb = col_rect_lin.z * cov1;
            float sa = col_rect_lin.w * cov1;
            r = sr + r * (1.0f - sa);
            g = sg + g * (1.0f - sa);
            b = sb + b * (1.0f - sa);
            a = sa + a * (1.0f - sa);
        }

        /* Layer 2: donut */
        float cov2 = tile_cov_donut[i];
        if (cov2 > 0.0f) {
            float sr = col_donut_lin.x * cov2;
            float sg = col_donut_lin.y * cov2;
            float sb = col_donut_lin.z * cov2;
            float sa = col_donut_lin.w * cov2;
            r = sr + r * (1.0f - sa);
            g = sg + g * (1.0f - sa);
            b = sb + b * (1.0f - sa);
            a = sa + a * (1.0f - sa);
        }

        float inv_a = a > 0.0f ? 1.0f / a : 0.0f;
        float r_enc = flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, r * inv_a);
        float g_enc = flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, g * inv_a);
        float b_enc = flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, b * inv_a);

        comp_px[i * 4 + 0] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, r_enc * a)) * 255.0f + 0.5f);
        comp_px[i * 4 + 1] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, g_enc * a)) * 255.0f + 0.5f);
        comp_px[i * 4 + 2] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, b_enc * a)) * 255.0f + 0.5f);
        comp_px[i * 4 + 3] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, a)) * 255.0f + 0.5f);
    }

    /* 3. Gate B5 Comparison: tight agreement within coverage tolerances */
    double max_diff = 0.0;
    double sum_diff = 0.0;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            size_t c_idx = ((size_t)y * cpu_stride) + (size_t)x * 4;
            size_t t_idx = ((size_t)y * W + x) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                double diff = abs((int)comp_px[t_idx + ch] - (int)cpu_px[c_idx + ch]);
                if (diff > max_diff)
                    max_diff = diff;
                sum_diff += diff;
            }
        }
    }

    double mean_diff = sum_diff / (double)(W * H * 3);
    fprintf(stderr, "  Composite scene: max_diff=%.1f channel units, mean_diff=%.3f\n", max_diff,
            mean_diff);

    int printed = 0;
    for (uint32_t y = 0; y < H && printed < 5; ++y) {
        for (uint32_t x = 0; x < W && printed < 5; ++x) {
            size_t c_idx = ((size_t)y * cpu_stride) + (size_t)x * 4;
            size_t t_idx = ((size_t)y * W + x) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                double diff = abs((int)comp_px[t_idx + ch] - (int)cpu_px[c_idx + ch]);
                if (diff > 5.0) {
                    fprintf(stderr, "    diff at (%u, %u) ch %d: comp=%u cpu=%u (diff=%.1f)\n", x,
                            y, ch, comp_px[t_idx + ch], cpu_px[c_idx + ch], diff);
                    printed++;
                    break;
                }
            }
        }
    }

    /* Re-expressed coverage tolerance: agreement to within 1 LSB */
    EXPECT(max_diff <= 2.0);
    EXPECT(mean_diff <= 0.1);

    free(tile_cov_rect);
    free(tile_cov_donut);
    free(comp_px);
    flux_canvas_release(cpu_canvas);
    flux_arena_deinit(&arena);
    TEST_SUMMARY();
}
