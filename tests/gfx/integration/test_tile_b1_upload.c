/*
 * Tile-compute coverage backend (ADR-0120 Phase B1 gate).
 *
 * Assert that covers uploaded into the GPU segment SSBO match the CPU
 * flattener's output exactly in segment count and endpoints.
 */
#include "gfx_test.h"
#include "optics_test.h"

#include "../../../libs/gfx/src/canvas/canvas_tile.h"
#include "../../../libs/gfx/src/canvas/internal.h"
#include <math.h>
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/vulkan.h>
#include <stdlib.h>
#include <string.h>

void optics_gfx_set_last_error(optics_gfx_result code, const char *function, const char *file,
                               int line, const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

#define MAX_TEST_SEGS 4096

static void test_shape_b1(optics_gfx_device *d, const optics_gfx_point *pts,
                          const optics_gfx_canvas_contour *cons, uint32_t ncons,
                          optics_gfx_fill_rule rule) {
    /* 1. CPU Reference extraction */
    optics_gfx_tile_segment ref_segs[MAX_TEST_SEGS];
    uint32_t ref_count = 0;
    for (uint32_t c = 0; c < ncons; ++c) {
        uint32_t n = cons[c].count;
        if (n < 2)
            continue;
        uint32_t start = cons[c].start;
        for (uint32_t k = 0; k < n; ++k) {
            optics_gfx_point a = pts[start + k];
            optics_gfx_point b = pts[start + (k + 1) % n];
            if (a.x == b.x && a.y == b.y)
                continue;
            if (ref_count < MAX_TEST_SEGS)
                ref_segs[ref_count++] = (optics_gfx_tile_segment){a.x, a.y, b.x, b.y};
        }
    }

    /* 2. Extraction via canvas_tile module */
    optics_gfx_tile_segment extracted[MAX_TEST_SEGS];
    optics_gfx_rect bounds;
    uint32_t ext_count =
        canvas_tile_extract_segments(pts, cons, ncons, extracted, MAX_TEST_SEGS, &bounds);
    OT_EXPECT(ext_count == ref_count);

    /* 3. Upload into GPU SSBO */
    optics_gfx_buffer *ssbo = nullptr;
    OT_EXPECT(canvas_tile_upload_segments(d, extracted, ext_count, rule, bounds, &ssbo) ==
              OPTICS_GFX_OK);
    OT_EXPECT(ssbo != nullptr);
    if (!ssbo)
        return;

    /* 4. Verification via GPU Buffer Address and Mapped Readback */
    uint64_t dev_addr = optics_gfx_buffer_device_address(ssbo);
    OT_EXPECT(dev_addr != 0);

    const uint8_t *mapped = (const uint8_t *)optics_gfx_buffer_mapped(ssbo);
    OT_EXPECT(mapped != nullptr);
    if (!mapped) {
        optics_gfx_buffer_release(ssbo);
        return;
    }

    const optics_gfx_tile_segment_header *hdr = (const optics_gfx_tile_segment_header *)mapped;
    OT_EXPECT(hdr->segment_count == ref_count);
    OT_EXPECT(hdr->fill_rule == (rule == OPTICS_GFX_FILL_EVEN_ODD ? 1u : 0u));
    OT_EXPECT_NEAR(hdr->bounds_min_x, bounds.x, 1e-5);
    OT_EXPECT_NEAR(hdr->bounds_min_y, bounds.y, 1e-5);
    OT_EXPECT_NEAR(hdr->bounds_max_x, bounds.x + bounds.w, 1e-5);
    OT_EXPECT_NEAR(hdr->bounds_max_y, bounds.y + bounds.h, 1e-5);

    const optics_gfx_tile_segment *gpu_segs =
        (const optics_gfx_tile_segment *)(mapped + sizeof(optics_gfx_tile_segment_header));

    for (uint32_t i = 0; i < ref_count; ++i) {
        OT_EXPECT_NEAR(gpu_segs[i].x0, ref_segs[i].x0, 1e-6);
        OT_EXPECT_NEAR(gpu_segs[i].y0, ref_segs[i].y0, 1e-6);
        OT_EXPECT_NEAR(gpu_segs[i].x1, ref_segs[i].x1, 1e-6);
        OT_EXPECT_NEAR(gpu_segs[i].y1, ref_segs[i].y1, 1e-6);
    }

    optics_gfx_buffer_release(ssbo);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_tile_b1_upload: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* Case 1: Simple quad */
    {
        optics_gfx_point sq[4] = {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}};
        optics_gfx_canvas_contour con = {.start = 0, .count = 4, .closed = true};
        test_shape_b1(d, sq, &con, 1, OPTICS_GFX_FILL_NON_ZERO);
    }

    /* Case 2: Circle through production flattener */
    {
        optics_gfx_arena arena;
        OT_EXPECT(optics_gfx_arena_init(&arena, 1u << 18, nullptr) == OPTICS_GFX_OK);
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_circle(p, 0.0f, 0.0f, 25.0f);

        static optics_gfx_point pts[OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP];
        static optics_gfx_canvas_contour cons[OPTICS_GFX_CANVAS_MAX_CONTOURS];
        flatten_multi fm = flatten_path_to_contours(
            p, 1.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP, cons, OPTICS_GFX_CANVAS_MAX_CONTOURS);
        OT_EXPECT(fm.contour_count == 1);
        OT_EXPECT(fm.point_count > 16);

        test_shape_b1(d, pts, cons, fm.contour_count, OPTICS_GFX_FILL_NON_ZERO);
        optics_gfx_arena_deinit(&arena);
    }

    /* Case 3: Self-intersecting star */
    {
        const int k = 5;
        optics_gfx_point v[5];
        for (int i = 0; i < k; ++i) {
            double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
            v[i] = (optics_gfx_point){(float)(20.0 * cos(a)), (float)(20.0 * sin(a))};
        }
        const int order[5] = {0, 2, 4, 1, 3};
        optics_gfx_point star[5];
        for (int i = 0; i < k; ++i)
            star[i] = v[order[i]];
        optics_gfx_canvas_contour con = {.start = 0, .count = 5, .closed = true};
        test_shape_b1(d, star, &con, 1, OPTICS_GFX_FILL_NON_ZERO);
        test_shape_b1(d, star, &con, 1, OPTICS_GFX_FILL_EVEN_ODD);
    }

    /* Case 4: Donut (nested contours with a hole) */
    {
        optics_gfx_point donut[8] = {
            {-20, -20}, {20, -20}, {20, 20}, {-20, 20}, {-10, -10}, {10, -10}, {10, 10}, {-10, 10},
        };
        optics_gfx_canvas_contour cons[2] = {
            {.start = 0, .count = 4, .closed = true},
            {.start = 4, .count = 4, .closed = true},
        };
        test_shape_b1(d, donut, cons, 2, OPTICS_GFX_FILL_EVEN_ODD);
    }

    /* Case 5: 1024-vertex high density polygon */
    {
        const uint32_t N = 1024;
        optics_gfx_point *poly = malloc((size_t)N * sizeof(optics_gfx_point));
        OT_EXPECT(poly != nullptr);
        if (poly) {
            for (uint32_t i = 0; i < N; ++i) {
                double a = (double)i * 2.0 * 3.14159265358979 / (double)N;
                poly[i] = (optics_gfx_point){(float)(100.0 * cos(a)), (float)(100.0 * sin(a))};
            }
            optics_gfx_canvas_contour con = {.start = 0, .count = N, .closed = true};
            test_shape_b1(d, poly, &con, 1, OPTICS_GFX_FILL_NON_ZERO);
            free(poly);
        }
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
