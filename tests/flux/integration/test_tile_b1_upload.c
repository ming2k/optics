/*
 * Tile-compute coverage backend (ADR-0120 Phase B1 gate).
 *
 * Assert that covers uploaded into the GPU segment SSBO match the CPU
 * flattener's output exactly in segment count and endpoints.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

#define MAX_TEST_SEGS 4096

static void test_shape_b1(flux_device *d, const flux_point *pts, const flux_canvas_contour *cons,
                          uint32_t ncons, flux_fill_rule rule) {
    /* 1. CPU Reference extraction */
    flux_tile_segment ref_segs[MAX_TEST_SEGS];
    uint32_t ref_count = 0;
    for (uint32_t c = 0; c < ncons; ++c) {
        uint32_t n = cons[c].count;
        if (n < 2)
            continue;
        uint32_t start = cons[c].start;
        for (uint32_t k = 0; k < n; ++k) {
            flux_point a = pts[start + k];
            flux_point b = pts[start + (k + 1) % n];
            if (a.x == b.x && a.y == b.y)
                continue;
            if (ref_count < MAX_TEST_SEGS)
                ref_segs[ref_count++] = (flux_tile_segment){a.x, a.y, b.x, b.y};
        }
    }

    /* 2. Extraction via canvas_tile module */
    flux_tile_segment extracted[MAX_TEST_SEGS];
    flux_rect bounds;
    uint32_t ext_count =
        canvas_tile_extract_segments(pts, cons, ncons, extracted, MAX_TEST_SEGS, &bounds);
    EXPECT(ext_count == ref_count);

    /* 3. Upload into GPU SSBO */
    flux_buffer *ssbo = nullptr;
    EXPECT(canvas_tile_upload_segments(d, extracted, ext_count, rule, bounds, &ssbo) == FLUX_OK);
    EXPECT(ssbo != nullptr);
    if (!ssbo)
        return;

    /* 4. Verification via GPU Buffer Address and Mapped Readback */
    uint64_t dev_addr = flux_buffer_device_address(ssbo);
    EXPECT(dev_addr != 0);

    const uint8_t *mapped = (const uint8_t *)flux_buffer_mapped(ssbo);
    EXPECT(mapped != nullptr);
    if (!mapped) {
        flux_buffer_release(ssbo);
        return;
    }

    const flux_tile_segment_header *hdr = (const flux_tile_segment_header *)mapped;
    EXPECT(hdr->segment_count == ref_count);
    EXPECT(hdr->fill_rule == (rule == FLUX_FILL_EVEN_ODD ? 1u : 0u));
    EXPECT_NEAR(hdr->bounds_min_x, bounds.x, 1e-5);
    EXPECT_NEAR(hdr->bounds_min_y, bounds.y, 1e-5);
    EXPECT_NEAR(hdr->bounds_max_x, bounds.x + bounds.w, 1e-5);
    EXPECT_NEAR(hdr->bounds_max_y, bounds.y + bounds.h, 1e-5);

    const flux_tile_segment *gpu_segs =
        (const flux_tile_segment *)(mapped + sizeof(flux_tile_segment_header));

    for (uint32_t i = 0; i < ref_count; ++i) {
        EXPECT_NEAR(gpu_segs[i].x0, ref_segs[i].x0, 1e-6);
        EXPECT_NEAR(gpu_segs[i].y0, ref_segs[i].y0, 1e-6);
        EXPECT_NEAR(gpu_segs[i].x1, ref_segs[i].x1, 1e-6);
        EXPECT_NEAR(gpu_segs[i].y1, ref_segs[i].y1, 1e-6);
    }

    flux_buffer_release(ssbo);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_tile_b1_upload: no Vulkan device; skipping\n");
        return 0;
    }

    /* Case 1: Simple quad */
    {
        flux_point sq[4] = {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}};
        flux_canvas_contour con = {.start = 0, .count = 4, .closed = true};
        test_shape_b1(d, sq, &con, 1, FLUX_FILL_NON_ZERO);
    }

    /* Case 2: Circle through production flattener */
    {
        flux_arena arena;
        EXPECT(flux_arena_init(&arena, 1u << 18, nullptr) == FLUX_OK);
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_add_circle(p, 0.0f, 0.0f, 25.0f);

        static flux_point pts[FLUX_CANVAS_PATH_SCRATCH_CAP];
        static flux_canvas_contour cons[FLUX_CANVAS_MAX_CONTOURS];
        flatten_multi fm = flatten_path_to_contours(p, 1.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP,
                                                    cons, FLUX_CANVAS_MAX_CONTOURS);
        EXPECT(fm.contour_count == 1);
        EXPECT(fm.point_count > 16);

        test_shape_b1(d, pts, cons, fm.contour_count, FLUX_FILL_NON_ZERO);
        flux_arena_deinit(&arena);
    }

    /* Case 3: Self-intersecting star */
    {
        const int k = 5;
        flux_point v[5];
        for (int i = 0; i < k; ++i) {
            double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
            v[i] = (flux_point){(float)(20.0 * cos(a)), (float)(20.0 * sin(a))};
        }
        const int order[5] = {0, 2, 4, 1, 3};
        flux_point star[5];
        for (int i = 0; i < k; ++i)
            star[i] = v[order[i]];
        flux_canvas_contour con = {.start = 0, .count = 5, .closed = true};
        test_shape_b1(d, star, &con, 1, FLUX_FILL_NON_ZERO);
        test_shape_b1(d, star, &con, 1, FLUX_FILL_EVEN_ODD);
    }

    /* Case 4: Donut (nested contours with a hole) */
    {
        flux_point donut[8] = {
            {-20, -20}, {20, -20}, {20, 20}, {-20, 20}, {-10, -10}, {10, -10}, {10, 10}, {-10, 10},
        };
        flux_canvas_contour cons[2] = {
            {.start = 0, .count = 4, .closed = true},
            {.start = 4, .count = 4, .closed = true},
        };
        test_shape_b1(d, donut, cons, 2, FLUX_FILL_EVEN_ODD);
    }

    /* Case 5: 1024-vertex high density polygon */
    {
        const uint32_t N = 1024;
        flux_point *poly = malloc((size_t)N * sizeof(flux_point));
        EXPECT(poly != nullptr);
        if (poly) {
            for (uint32_t i = 0; i < N; ++i) {
                double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
                poly[i] = (flux_point){(float)(100.0 * cos(a)), (float)(100.0 * sin(a))};
            }
            flux_canvas_contour con = {.start = 0, .count = N, .closed = true};
            test_shape_b1(d, poly, &con, 1, FLUX_FILL_NON_ZERO);
            free(poly);
        }
    }

    flux_device_release(d);
    TEST_SUMMARY();
}
