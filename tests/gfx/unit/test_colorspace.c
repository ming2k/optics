/*
 * Color-space model tests (ADR-0069).
 *
 *   - transfer functions: definitional anchors + round-trips
 *     (PQ anchor: ST 2084 code 0.751827 == 1000 cd/m² == 0.1 linear)
 *   - validity / equality incl. the scRGB == sRGB-linear parametric
 *     identity
 *   - mat3: multiply / transform / invert round-trip
 *   - transform matrices: identity for same space, white preservation
 *     across D65 spaces, gamut containment directions, Bradford
 *     adaptation mapping white to white, A->B->A round-trips
 */
#include "optics_test.h"
#include <optics/gfx.h>

#define EPS 1e-4f

static bool mat3_near(optics_gfx_mat3 a, optics_gfx_mat3 b, float eps) {
    for (int i = 0; i < 9; ++i) {
        float d = a.m[i] - b.m[i];
        if (d < 0)
            d = -d;
        if (d > eps)
            return false;
    }
    return true;
}

static bool vec3_near(optics_gfx_vec3 a, optics_gfx_vec3 b, float eps) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    if (dz < 0)
        dz = -dz;
    return dx <= eps && dy <= eps && dz <= eps;
}

static void expect_round_trip(optics_gfx_transfer_func tf, float gamma, float x) {
    float back = optics_gfx_transfer_decode(tf, gamma, optics_gfx_transfer_encode(tf, gamma, x));
    OT_EXPECT_NEAR(back, x, 2e-4f);
}

int main(void) {
    const optics_gfx_color_space srgb = OPTICS_GFX_COLOR_SPACE_SRGB;
    const optics_gfx_color_space scrgb = OPTICS_GFX_COLOR_SPACE_SCRGB;
    const optics_gfx_color_space srgb_linear = OPTICS_GFX_COLOR_SPACE_SRGB_LINEAR;
    const optics_gfx_color_space p3 = OPTICS_GFX_COLOR_SPACE_DISPLAY_P3;
    const optics_gfx_color_space p3_linear = OPTICS_GFX_COLOR_SPACE_DISPLAY_P3_LINEAR;
    const optics_gfx_color_space adobe = OPTICS_GFX_COLOR_SPACE_ADOBE_RGB;
    const optics_gfx_color_space bt2020 = OPTICS_GFX_COLOR_SPACE_BT2020;
    const optics_gfx_color_space bt2020_pq = OPTICS_GFX_COLOR_SPACE_BT2020_PQ;
    const optics_gfx_color_space bt2020_hlg = OPTICS_GFX_COLOR_SPACE_BT2020_HLG;

    /* --- transfer: sRGB definitional anchors --- */
    {
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, 1.0f), 1.0f, EPS);
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, 0.0f), 0.0f, EPS);
        /* The piecewise knee is exact by definition. */
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, 0.04045f),
                       0.04045f / 12.92f, EPS);
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, 0.0031308f),
                       0.04045f, 1e-3f);
    }

    /* --- transfer: PQ external anchor (ST 2084) --- */
    {
        /* 1000 cd/m² is linear 0.1 in units of 10000 nits and encodes
         * to the published code value 0.751827. */
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_PQ, 0.0f, 0.1f), 0.751827f,
                       1e-3f);
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_PQ, 0.0f, 0.751827f), 0.1f,
                       1e-3f);
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_PQ, 0.0f, 0.0f), 0.0f, EPS);
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_PQ, 0.0f, 1.0f), 1.0f, EPS);
    }

    /* --- transfer: HLG definitional anchors --- */
    {
        /* Knee at scene-linear 1/12 encodes to exactly 0.5. */
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_HLG, 0.0f, 1.0f / 12.0f),
                       0.5f, EPS);
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_HLG, 0.0f, 0.5f),
                       1.0f / 12.0f, EPS);
        OT_EXPECT_NEAR(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_HLG, 0.0f, 1.0f), 1.0f, EPS);
    }

    /* --- transfer: round trips across the whole set --- */
    {
        static const float xs[] = {0.001f, 0.01f, 0.05f, 0.18f, 0.5f, 0.9f, 1.0f};
        for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
            expect_round_trip(OPTICS_GFX_TRANSFER_LINEAR, 0.0f, xs[i]);
            expect_round_trip(OPTICS_GFX_TRANSFER_SRGB, 0.0f, xs[i]);
            expect_round_trip(OPTICS_GFX_TRANSFER_GAMMA, 2.2f, xs[i]);
            expect_round_trip(OPTICS_GFX_TRANSFER_GAMMA, 2.4f, xs[i]);
            expect_round_trip(OPTICS_GFX_TRANSFER_PQ, 0.0f, xs[i]);
            expect_round_trip(OPTICS_GFX_TRANSFER_HLG, 0.0f, xs[i]);
        }
        /* Gamma spot check: 0.5^2.2 */
        OT_EXPECT_NEAR(optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_GAMMA, 2.2f, 0.5f), 0.217637f,
                       1e-3f);
    }

    /* --- validity / equality --- */
    {
        OT_EXPECT(optics_gfx_color_space_is_valid(srgb));
        OT_EXPECT(optics_gfx_color_space_is_valid(scrgb));
        OT_EXPECT(optics_gfx_color_space_is_valid(p3));
        OT_EXPECT(optics_gfx_color_space_is_valid(adobe));
        OT_EXPECT(optics_gfx_color_space_is_valid(bt2020));
        OT_EXPECT(optics_gfx_color_space_is_valid(bt2020_pq));
        OT_EXPECT(optics_gfx_color_space_is_valid(bt2020_hlg));

        OT_EXPECT(!optics_gfx_color_space_is_valid((optics_gfx_color_space){
            OPTICS_GFX_PRIMARIES_BT709, OPTICS_GFX_TRANSFER_GAMMA, 0.0f, {}}));
        OT_EXPECT(!optics_gfx_color_space_is_valid(
            (optics_gfx_color_space){99, OPTICS_GFX_TRANSFER_SRGB, 0.0f, {}}));
        OT_EXPECT(!optics_gfx_color_space_is_valid(
            (optics_gfx_color_space){OPTICS_GFX_PRIMARIES_BT709, 99, 0.0f, {}}));

        /* A real custom space: DCI-P3 theatre white (greenish). */
        optics_gfx_color_space dci = {
            OPTICS_GFX_PRIMARIES_CUSTOM,
            OPTICS_GFX_TRANSFER_GAMMA,
            2.6f,
            {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.314f, 0.351f}};
        OT_EXPECT(optics_gfx_color_space_is_valid(dci));
        /* Degenerate triangle: red == green. */
        optics_gfx_color_space bad = dci;
        bad.xy.gx = bad.xy.rx;
        bad.xy.gy = bad.xy.ry;
        OT_EXPECT(!optics_gfx_color_space_is_valid(bad));

        OT_EXPECT(optics_gfx_color_space_equal(srgb, srgb));
        OT_EXPECT(!optics_gfx_color_space_equal(srgb, p3));
        /* scRGB and sRGB-linear are the same {primaries, transfer} —
         * the presets differ only in surface intent. */
        OT_EXPECT(optics_gfx_color_space_equal(scrgb, srgb_linear));
        /* Same primaries, different gamma: not equal. */
        OT_EXPECT(!optics_gfx_color_space_equal(
            (optics_gfx_color_space){
                OPTICS_GFX_PRIMARIES_BT2020, OPTICS_GFX_TRANSFER_GAMMA, 2.4f, {}},
            (optics_gfx_color_space){
                OPTICS_GFX_PRIMARIES_BT2020, OPTICS_GFX_TRANSFER_GAMMA, 2.2f, {}}));
    }

    /* --- mat3 --- */
    {
        optics_gfx_mat3 id = optics_gfx_mat3_identity();
        optics_gfx_vec3 v = {0.3f, -0.7f, 2.0f};
        OT_EXPECT(vec3_near(optics_gfx_mat3_transform_vec3(id, v), v, EPS));

        optics_gfx_mat3 diag = {{2, 0, 0, 0, 4, 0, 0, 0, 8}};
        optics_gfx_mat3 diag_inv = optics_gfx_mat3_invert(diag);
        OT_EXPECT_NEAR(diag_inv.m[0], 0.5f, EPS);
        OT_EXPECT_NEAR(diag_inv.m[4], 0.25f, EPS);
        OT_EXPECT_NEAR(diag_inv.m[8], 0.125f, EPS);
        OT_EXPECT(mat3_near(optics_gfx_mat3_multiply(diag, diag_inv), id, EPS));

        /* Associativity sample with a non-symmetric matrix. */
        optics_gfx_mat3 a = {{1, 4, 7, 2, 5, 8, 3, 6, 10}};
        optics_gfx_mat3 b = {{0, 1, 0, 1, 0, 0, 2, 0, 1}};
        OT_EXPECT(mat3_near(optics_gfx_mat3_multiply(optics_gfx_mat3_multiply(a, b), diag),
                            optics_gfx_mat3_multiply(a, optics_gfx_mat3_multiply(b, diag)), 1e-3f));
    }

    /* --- transform matrices --- */
    {
        optics_gfx_mat3 m;

        /* Same space -> identity. */
        OT_EXPECT(optics_gfx_color_space_transform_matrix(srgb, srgb, &m));
        OT_EXPECT(mat3_near(m, optics_gfx_mat3_identity(), EPS));

        /* D65 -> D65 preserves white exactly. */
        OT_EXPECT(optics_gfx_color_space_transform_matrix(srgb, p3, &m));
        OT_EXPECT(vec3_near(optics_gfx_mat3_transform_vec3(m, (optics_gfx_vec3){1, 1, 1}),
                            (optics_gfx_vec3){1, 1, 1}, 1e-3f));

        /* P3 red is out of sRGB gamut: r > 1, g < 0. */
        OT_EXPECT(optics_gfx_color_space_transform_matrix(p3, srgb, &m));
        optics_gfx_vec3 p3_red_in_srgb =
            optics_gfx_mat3_transform_vec3(m, (optics_gfx_vec3){1, 0, 0});
        OT_EXPECT(p3_red_in_srgb.x > 1.0f);
        OT_EXPECT(p3_red_in_srgb.y < 0.0f);

        /* BT.2020 essentially contains P3: every primary lands in [0,1]
         * up to the published matrices' small negative lobes (the P3 ->
         * BT.2020 blue row starts at -0.0012; P3 red sits a hair past
         * the BT.2020 blue corner). */
        OT_EXPECT(optics_gfx_color_space_transform_matrix(p3, bt2020_pq, &m));
        for (int c = 0; c < 3; ++c) {
            optics_gfx_vec3 p = {0, 0, 0};
            ((float *)&p)[c] = 1.0f;
            optics_gfx_vec3 q = optics_gfx_mat3_transform_vec3(m, p);
            OT_EXPECT(q.x >= -2.5e-3f && q.x <= 1.0f);
            OT_EXPECT(q.y >= -2.5e-3f && q.y <= 1.0f);
            OT_EXPECT(q.z >= -2.5e-3f && q.z <= 1.0f);
        }

        /* A -> B -> A round-trips. */
        optics_gfx_mat3 fwd, back, rt;
        OT_EXPECT(optics_gfx_color_space_transform_matrix(srgb, adobe, &fwd));
        OT_EXPECT(optics_gfx_color_space_transform_matrix(adobe, srgb, &back));
        rt = optics_gfx_mat3_multiply(back, fwd);
        OT_EXPECT(mat3_near(rt, optics_gfx_mat3_identity(), 1e-3f));
        OT_EXPECT(optics_gfx_color_space_transform_matrix(p3_linear, bt2020_hlg, &fwd));
        OT_EXPECT(optics_gfx_color_space_transform_matrix(bt2020_hlg, p3_linear, &back));
        rt = optics_gfx_mat3_multiply(back, fwd);
        OT_EXPECT(mat3_near(rt, optics_gfx_mat3_identity(), 1e-3f));

        /* Bradford: D50-white custom space -> D65 sRGB maps white to white. */
        optics_gfx_color_space d50_space = {
            OPTICS_GFX_PRIMARIES_CUSTOM,
            OPTICS_GFX_TRANSFER_SRGB,
            0.0f,
            {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3457f, 0.3585f}};
        OT_EXPECT(optics_gfx_color_space_transform_matrix(d50_space, srgb, &m));
        OT_EXPECT(vec3_near(optics_gfx_mat3_transform_vec3(m, (optics_gfx_vec3){1, 1, 1}),
                            (optics_gfx_vec3){1, 1, 1}, 1e-3f));
        /* ...but is not the identity — adaptation actually happened. */
        OT_EXPECT(!mat3_near(m, optics_gfx_mat3_identity(), 1e-3f));

        /* Invalid input fails and yields identity. */
        OT_EXPECT(!optics_gfx_color_space_transform_matrix(
            (optics_gfx_color_space){
                OPTICS_GFX_PRIMARIES_BT709, OPTICS_GFX_TRANSFER_GAMMA, 0.0f, {}},
            srgb, &m));
        OT_EXPECT(mat3_near(m, optics_gfx_mat3_identity(), EPS));
    }

    return OT_REPORT();
}
