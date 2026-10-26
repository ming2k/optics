/*
 * Unit test for Algorithm Vault pure ear-clip tessellator asset (ADR-0121).
 */
#include "test_helpers.h"
#include <flux/vault/tess_earcut.h>
#include <math.h>

static float triangle_area(flux_point a, flux_point b, flux_point c) {
    return 0.5f * fabsf((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

int main(void) {
    // 1. Degenerate and argument tests
    EXPECT(flux_vault_earcut_triangulate(nullptr, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_point deg_pts[2] = {{0, 0}, {10, 10}};
    uint32_t indices[16];
    flux_vault_earcut_desc desc = {
        .points = deg_pts,
        .point_count = 2,
        .out_indices = indices,
        .index_capacity = 16,
    };
    EXPECT(flux_vault_earcut_triangulate(&desc, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);

    // 2. Simple convex quad (CCW)
    flux_point quad_pts[4] = {
        {0.0f, 0.0f},
        {100.0f, 0.0f},
        {100.0f, 100.0f},
        {0.0f, 100.0f},
    };
    desc.points = quad_pts;
    desc.point_count = 4;
    desc.index_capacity = 6;
    flux_vault_earcut_result res = {0};
    EXPECT(flux_vault_earcut_triangulate(&desc, &res) == FLUX_OK);
    EXPECT(res.triangle_count == 2);
    EXPECT(res.index_count == 6);
    EXPECT(!res.is_clockwise);

    float total_area = 0.0f;
    for (uint32_t i = 0; i < res.triangle_count; ++i) {
        flux_point a = quad_pts[indices[i * 3 + 0]];
        flux_point b = quad_pts[indices[i * 3 + 1]];
        flux_point c = quad_pts[indices[i * 3 + 2]];
        total_area += triangle_area(a, b, c);
    }
    EXPECT(fabsf(total_area - 10000.0f) < 1e-4f);

    // 3. Concave L-shape polygon (6 vertices)
    flux_point l_pts[6] = {
        {0.0f, 0.0f},    {50.0f, 0.0f},    {50.0f, 50.0f},
        {100.0f, 50.0f}, {100.0f, 100.0f}, {0.0f, 100.0f},
    };
    uint32_t l_indices[32];
    desc.points = l_pts;
    desc.point_count = 6;
    desc.out_indices = l_indices;
    desc.index_capacity = 32;
    EXPECT(flux_vault_earcut_triangulate(&desc, &res) == FLUX_OK);
    EXPECT(res.triangle_count == 4);
    EXPECT(res.index_count == 12);

    float l_area = 0.0f;
    for (uint32_t i = 0; i < res.triangle_count; ++i) {
        flux_point a = l_pts[l_indices[i * 3 + 0]];
        flux_point b = l_pts[l_indices[i * 3 + 1]];
        flux_point c = l_pts[l_indices[i * 3 + 2]];
        l_area += triangle_area(a, b, c);
    }
    // L-shape expected area: 100*100 - 50*50 = 7500
    EXPECT(fabsf(l_area - 7500.0f) < 1e-4f);

    return 0;
}
