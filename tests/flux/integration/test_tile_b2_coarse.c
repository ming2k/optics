/*
 * Tile-compute coarse rasterizer (ADR-0120 Phase B2 gate).
 *
 * Assert that per-tile segment sets match a CPU computation of the same binning
 * across 16x16 tiles.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include <flux/flux.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 256u
#define H 256u
#define MAX_INDICES 65536u

static int cmp_u32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static void verify_binning(const flux_tile_segment *segs, uint32_t count, uint32_t width,
                           uint32_t height) {
    uint32_t tx = 0, ty = 0;
    canvas_tile_grid_dims(width, height, &tx, &ty);
    uint32_t total_tiles = tx * ty;

    flux_tile_desc *tiles = calloc(total_tiles, sizeof(flux_tile_desc));
    uint32_t *indices = malloc(MAX_INDICES * sizeof(uint32_t));
    EXPECT(tiles && indices);
    if (!tiles || !indices) {
        free(tiles);
        free(indices);
        return;
    }

    uint32_t total_indices = 0;
    EXPECT(canvas_tile_bin_segments(segs, count, width, height, tiles, indices, MAX_INDICES,
                                    &total_indices) == FLUX_OK);

    /* Independent direct computation per tile */
    for (uint32_t y = 0; y < ty; ++y) {
        float tile_y0 = (float)(y * FLUX_TILE_SIZE);
        float tile_y1 = tile_y0 + (float)FLUX_TILE_SIZE;
        for (uint32_t x = 0; x < tx; ++x) {
            float tile_x0 = (float)(x * FLUX_TILE_SIZE);
            float tile_x1 = tile_x0 + (float)FLUX_TILE_SIZE;

            uint32_t tidx = y * tx + x;
            uint32_t expected_count = 0;
            uint32_t expected_idx[256];

            for (uint32_t i = 0; i < count; ++i) {
                if (canvas_tile_segment_intersects(segs[i], tile_x0, tile_y0, tile_x1, tile_y1)) {
                    if (expected_count < 256)
                        expected_idx[expected_count++] = i;
                }
            }

            /* Gate B2: segment count matches exactly */
            EXPECT(tiles[tidx].segment_count == expected_count);

            /* Gate B2: segment sets match exactly */
            if (expected_count > 0 && expected_count < 256) {
                uint32_t offset = tiles[tidx].segment_offset;
                uint32_t got_idx[256];
                memcpy(got_idx, &indices[offset], expected_count * sizeof(uint32_t));

                qsort(got_idx, expected_count, sizeof(uint32_t), cmp_u32);
                qsort(expected_idx, expected_count, sizeof(uint32_t), cmp_u32);

                for (uint32_t k = 0; k < expected_count; ++k)
                    EXPECT(got_idx[k] == expected_idx[k]);
            }
        }
    }

    free(tiles);
    free(indices);
}

int main(void) {
    /* 1. Unit intersection edge cases */
    {
        /* Strictly outside */
        EXPECT(!canvas_tile_segment_intersects((flux_tile_segment){0, 0, 10, 10}, 20, 20, 36, 36));
        /* Strictly inside */
        EXPECT(canvas_tile_segment_intersects((flux_tile_segment){22, 22, 28, 28}, 20, 20, 36, 36));
        /* Diagonal cutting cleanly through */
        EXPECT(canvas_tile_segment_intersects((flux_tile_segment){10, 10, 40, 40}, 20, 20, 36, 36));
        /* Passing near corner and cutting through (15, 30) -> (30, 15) cuts [20, 36)x[20, 36) */
        EXPECT(canvas_tile_segment_intersects((flux_tile_segment){15, 30, 30, 15}, 20, 20, 36, 36));
        /* Strictly outside near corner without touching */
        EXPECT(!canvas_tile_segment_intersects((flux_tile_segment){5, 20, 15, 10}, 20, 20, 36, 36));
        /* Horizontal crossing */
        EXPECT(canvas_tile_segment_intersects((flux_tile_segment){10, 25, 45, 25}, 20, 20, 36, 36));
        /* Vertical crossing */
        EXPECT(canvas_tile_segment_intersects((flux_tile_segment){25, 10, 25, 45}, 20, 20, 36, 36));
    }

    /* 2. Rotated oblique edge (crosses multiple tile phases) */
    {
        flux_tile_segment oblique[1] = {{12.0f, 15.0f, 230.0f, 198.0f}};
        verify_binning(oblique, 1, W, H);
    }

    /* 3. Convex rectangle */
    {
        flux_tile_segment box[4] = {
            {32.0f, 32.0f, 96.0f, 32.0f},
            {96.0f, 32.0f, 96.0f, 96.0f},
            {96.0f, 96.0f, 32.0f, 96.0f},
            {32.0f, 96.0f, 32.0f, 32.0f},
        };
        verify_binning(box, 4, W, H);
    }

    /* 4. Self-intersecting 5-pointed star */
    {
        const int k = 5;
        flux_point v[5];
        for (int i = 0; i < k; ++i) {
            double a = -1.5707963 + (double)i * (2.0 * 3.14159265358979 / (double)k);
            v[i] = (flux_point){(float)(128.0 + 80.0 * cos(a)), (float)(128.0 + 80.0 * sin(a))};
        }
        const int order[5] = {0, 2, 4, 1, 3};
        flux_tile_segment star[5];
        for (int i = 0; i < k; ++i) {
            flux_point p0 = v[order[i]];
            flux_point p1 = v[order[(i + 1) % k]];
            star[i] = (flux_tile_segment){p0.x, p0.y, p1.x, p1.y};
        }
        verify_binning(star, 5, W, H);
    }

    /* 5. High density 128-gon polygon */
    {
        const uint32_t N = 128;
        flux_tile_segment poly[128];
        for (uint32_t i = 0; i < N; ++i) {
            double a0 = (double)i * 2.0 * 3.14159265358979 / (double)N;
            double a1 = (double)(i + 1) * 2.0 * 3.14159265358979 / (double)N;
            poly[i] = (flux_tile_segment){
                (float)(128.0 + 90.0 * cos(a0)),
                (float)(128.0 + 90.0 * sin(a0)),
                (float)(128.0 + 90.0 * cos(a1)),
                (float)(128.0 + 90.0 * sin(a1)),
            };
        }
        verify_binning(poly, N, W, H);
    }

    TEST_SUMMARY();
}
