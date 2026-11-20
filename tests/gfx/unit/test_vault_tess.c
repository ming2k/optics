/*
 * Unit test for Algorithm Vault pure ear-clip tessellator asset (ADR-0121).
 */
#include "optics_test.h"
#include <math.h>
#include <optics/gfx/vault/tess_earcut.h>

static float triangle_area(optics_gfx_point a, optics_gfx_point b, optics_gfx_point c) {
    return 0.5f * fabsf((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

int main(void) {
    // 1. Degenerate and argument tests
    OT_EXPECT(optics_gfx_vault_earcut_triangulate(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_point deg_pts[2] = {{0, 0}, {10, 10}};
    uint32_t indices[16];
    optics_gfx_vault_earcut_desc desc = {
        .points = deg_pts,
        .point_count = 2,
        .out_indices = indices,
        .index_capacity = 16,
    };
    OT_EXPECT(optics_gfx_vault_earcut_triangulate(&desc, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    // 2. Simple convex quad (CCW)
    optics_gfx_point quad_pts[4] = {
        {0.0f, 0.0f},
        {100.0f, 0.0f},
        {100.0f, 100.0f},
        {0.0f, 100.0f},
    };
    desc.points = quad_pts;
    desc.point_count = 4;
    desc.index_capacity = 6;
    optics_gfx_vault_earcut_result res = {};
    OT_EXPECT(optics_gfx_vault_earcut_triangulate(&desc, &res) == OPTICS_GFX_OK);
    OT_EXPECT(res.triangle_count == 2);
    OT_EXPECT(res.index_count == 6);
    OT_EXPECT(!res.is_clockwise);

    float total_area = 0.0f;
    for (uint32_t i = 0; i < res.triangle_count; ++i) {
        optics_gfx_point a = quad_pts[indices[i * 3 + 0]];
        optics_gfx_point b = quad_pts[indices[i * 3 + 1]];
        optics_gfx_point c = quad_pts[indices[i * 3 + 2]];
        total_area += triangle_area(a, b, c);
    }
    OT_EXPECT(fabsf(total_area - 10000.0f) < 1e-4f);

    // 3. Concave L-shape polygon (6 vertices)
    optics_gfx_point l_pts[6] = {
        {0.0f, 0.0f},    {50.0f, 0.0f},    {50.0f, 50.0f},
        {100.0f, 50.0f}, {100.0f, 100.0f}, {0.0f, 100.0f},
    };
    uint32_t l_indices[32];
    desc.points = l_pts;
    desc.point_count = 6;
    desc.out_indices = l_indices;
    desc.index_capacity = 32;
    OT_EXPECT(optics_gfx_vault_earcut_triangulate(&desc, &res) == OPTICS_GFX_OK);
    OT_EXPECT(res.triangle_count == 4);
    OT_EXPECT(res.index_count == 12);

    float l_area = 0.0f;
    for (uint32_t i = 0; i < res.triangle_count; ++i) {
        optics_gfx_point a = l_pts[l_indices[i * 3 + 0]];
        optics_gfx_point b = l_pts[l_indices[i * 3 + 1]];
        optics_gfx_point c = l_pts[l_indices[i * 3 + 2]];
        l_area += triangle_area(a, b, c);
    }
    // L-shape expected area: 100*100 - 50*50 = 7500
    OT_EXPECT(fabsf(l_area - 7500.0f) < 1e-4f);

    return 0;
}
