/* fuzz_coverage_raster.c — feed random bytes as polygon contours to coverage_rasterize.
 *
 * Interprets bytes as points (x, y) forming closed contours on a bounded grid,
 * stressing the active edge table, snapping, sub-band splitting, and area accumulation.
 */

#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/core.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FUZZ_POINTS 512
#define GRID_W 64u
#define GRID_H 64u

static float read_f32(const uint8_t *p, size_t avail, size_t *cursor) {
    uint8_t b[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (*cursor + i < avail)
            b[i] = p[*cursor + i];
    }
    *cursor += 4;
    uint32_t u =
        (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    float f;
    memcpy(&f, &u, sizeof f);
    if (!isfinite(f))
        return 0.0f;
    /* Map to a bounded coordinate range around [-100, 100] */
    if (fabsf(f) > 100.0f)
        return copysignf(fmodf(fabsf(f), 100.0f), f);
    return f;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 8)
        return 0;

    size_t cursor = 0;
    uint8_t flags = data[cursor++];
    flux_fill_rule rule = (flags & 1) ? FLUX_FILL_EVEN_ODD : FLUX_FILL_NON_ZERO;

    flux_point pts[MAX_FUZZ_POINTS];
    uint32_t npts = 0;

    while (cursor + 8 <= size && npts < MAX_FUZZ_POINTS) {
        pts[npts].x = read_f32(data, size, &cursor);
        pts[npts].y = read_f32(data, size, &cursor);
        npts++;
    }

    if (npts < 3)
        return 0;

    float *out_cov = malloc((size_t)GRID_W * GRID_H * sizeof(float));
    if (!out_cov)
        return 0;

    flux_canvas_contour con = {
        .start = 0,
        .count = npts,
        .closed = true,
    };

    flux_result rc =
        canvas_coverage_rasterize(pts, &con, 1, rule, -32.0f, -32.0f, GRID_W, GRID_H, out_cov);
    if (rc == FLUX_OK) {
        for (size_t i = 0; i < (size_t)GRID_W * GRID_H; ++i) {
            float c = out_cov[i];
            /* Assert that coverage is strictly bounded in [0.0, 1.0] */
            if (c < 0.0f || c > 1.0f) {
                free(out_cov);
                abort();
            }
        }
    }

    free(out_cov);
    return 0;
}
