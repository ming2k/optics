/*
 * Squircle path ↔ field agreement (ADR-0118 INV-SDF-06).
 *
 * This closes the schism the ADR records: a squircle used to be two different
 * shapes — an Apple-style cubic path in flux and an L2/L4 superellipse field
 * in Prism — so the canvas and the materials disagreed for the same
 * `curvature`. Now `flux_path_add_squircle` emits two cubics per corner fitted
 * to `flux_sdf_squircle`'s zero set, and this test asserts that every point of
 * the fitted path actually lies on that zero set.
 *
 * The claim is analytic, not golden-image: for a point p on the path,
 * |flux_sdf_squircle(p − centre, hw, hh, r, c)| is (to within the field's unit
 * gradient) p's distance from the shape the field describes. The measured worst
 * case is 0.12% of the corner radius, and the bound asserted here is 1%.
 *
 * Also pinned: curvature 0 is exactly the rounded rectangle (so introducing
 * curvature never jumps), the corner's diagonal bulge equals the field's
 * diagonal radius at every curvature, and the straight edges are untouched.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/internal.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>
#include <flux/flux.h>
#include <flux/sdf.h>

#include <math.h>
#include <stdio.h>

#define PATH_POINTS 8192
#define LINE_SUBDIV 8

static flux_point g_pts[PATH_POINTS];

/* Sample a path's outline, interpolating lines and curves. */
static uint32_t sample_path(const flux_path *p, flux_point *out, uint32_t cap) {
    uint32_t n = 0;
    flux_point cur = {0.0f, 0.0f};
    flux_point start = {0.0f, 0.0f};
    for (uint32_t i = 0; i < p->count && n < cap; ++i) {
        const flux_path_segment *s = &p->segments[i];
        switch (s->op) {
        case FLUX_PATH_MOVE:
            cur = (flux_point){s->pts[0], s->pts[1]};
            start = cur;
            out[n++] = cur;
            break;
        case FLUX_PATH_LINE: {
            flux_point e = {s->pts[0], s->pts[1]};
            for (int k = 1; k <= LINE_SUBDIV && n < cap; ++k) {
                float t = (float)k / (float)LINE_SUBDIV;
                out[n++] = (flux_point){cur.x + (e.x - cur.x) * t, cur.y + (e.y - cur.y) * t};
            }
            cur = e;
            break;
        }
        case FLUX_PATH_QUAD: {
            flux_curve_seg q =
                flux_curve_seg_make(FLUX_CURVE_QUAD, cur, (flux_point){s->pts[0], s->pts[1]},
                                    (flux_point){s->pts[2], s->pts[3]});
            for (int k = 1; k <= 16 && n < cap; ++k)
                out[n++] = flux_curve_quad_eval(q, (float)k / 16.0f);
            cur = (flux_point){s->pts[2], s->pts[3]};
            break;
        }
        case FLUX_PATH_CUBIC: {
            flux_point c1 = {s->pts[0], s->pts[1]};
            flux_point c2 = {s->pts[2], s->pts[3]};
            flux_point e = {s->pts[4], s->pts[5]};
            for (int k = 1; k <= 16 && n < cap; ++k)
                out[n++] = flux_curve_cubic_eval(cur, c1, c2, e, (float)k / 16.0f);
            cur = e;
            break;
        }
        case FLUX_PATH_CLOSE:
            cur = start;
            break;
        }
    }
    return n;
}

/* Worst |field| over the path's points: how far the drawn outline is from the
 * shape the field describes. */
static float path_field_deviation(const flux_path *p, flux_point centre, flux_rect r, float radius,
                                  float curvature) {
    uint32_t n = sample_path(p, g_pts, PATH_POINTS);
    float hw = r.w * 0.5f, hh = r.h * 0.5f;
    float worst = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float d = flux_sdf_squircle((flux_point){g_pts[i].x - centre.x, g_pts[i].y - centre.y}, hw,
                                    hh, radius, curvature);
        if (fabsf(d) > worst)
            worst = fabsf(d);
    }
    return worst;
}

/* Distance from the top-right corner's circle centre to the path along the
 * corner diagonal: the field's diagonal radius, in pixels. */
static float diagonal_bulge(const flux_path *p, flux_point corner_centre) {
    uint32_t n = sample_path(p, g_pts, PATH_POINTS);
    const float inv_sqrt2 = 0.70710678f;
    float best = -1.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float dx = g_pts[i].x - corner_centre.x;
        float dy = g_pts[i].y - corner_centre.y;
        /* `along` is ~0 on the corner diagonal; `across` is the radial
         * distance along it. (For the top-right corner the diagonal runs
         * +x/−y, so it is the difference, not the sum.) */
        float along = (dx + dy) * inv_sqrt2;
        float across = (dx - dy) * inv_sqrt2;
        if (fabsf(along) < 0.75f && across > best)
            best = across;
    }
    return best;
}

/* --- Analytic dispatch, end to end (ADR-0118 INV-SDF-03).
 *
 * The same squircle is drawn twice through the CPU canvas: once as
 * FLUX_GEOM_SQUIRCLE, which now takes the analytic SDF path, and once as the
 * equivalent path, which goes through flatten + tessellate + sampled AA. Two
 * independent code paths producing the same coverage is the evidence that the
 * dispatch change is correct — and, specifically, that the software backend's
 * SDF host port really evaluates the shared field (an earlier revision of this
 * work claimed it did while the file still held a private copy of the
 * formula, which no test caught).
 *
 * Coverage is compared as an ink mass plus an inside/outside agreement count,
 * both robust to the two paths' different anti-aliasing strategies. */
static void test_analytic_dispatch_matches_path(void) {
    const flux_rect box = {24.0f, 20.0f, 96.0f, 72.0f};
    const float radius = 20.0f;
    const float curvatures[] = {0.0f, 0.5f, 1.0f};

    for (unsigned k = 0; k < sizeof(curvatures) / sizeof(curvatures[0]); ++k) {
        float curv = curvatures[k];
        flux_canvas *analytic = nullptr;
        flux_canvas *tessellated = nullptr;
        EXPECT(flux_canvas_create_cpu(144, 112, 1.0f, &analytic) == FLUX_OK);
        EXPECT(flux_canvas_create_cpu(144, 112, 1.0f, &tessellated) == FLUX_OK);
        if (!analytic || !tessellated)
            return;

        flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
        flux_brush brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 255));
        flux_geometry geom = flux_geom_squircle(box, radius, curv);

        /* Analytic: the geometry takes the SDF pipeline. */
        EXPECT(flux_canvas_begin(analytic,
                                 &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == FLUX_OK);
        flux_canvas_draw_geometry(analytic, &geom, &brush);
        EXPECT(flux_canvas_end(analytic) == FLUX_OK);

        /* Tessellated: force the path route by drawing a gradient-free but
         * non-solid-equivalent paint? No — draw the path explicitly, which is
         * the independent pipeline. */
        flux_arena local;
        EXPECT(flux_arena_init(&local, 1u << 16, nullptr) == FLUX_OK);
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &local) == FLUX_OK);
        flux_path_add_squircle(p, box, radius, curv);
        EXPECT(flux_canvas_begin(tessellated,
                                 &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == FLUX_OK);
        flux_canvas_fill_path(tessellated, p, FLUX_FILL_NON_ZERO, &brush);
        EXPECT(flux_canvas_end(tessellated) == FLUX_OK);

        uint32_t w = 0, h = 0;
        const uint8_t *a = flux_canvas_cpu_pixels(analytic, &w, &h, nullptr);
        const uint8_t *b = flux_canvas_cpu_pixels(tessellated, &w, &h, nullptr);
        EXPECT(a != nullptr && b != nullptr);
        if (a && b) {
            long ink_analytic = 0, ink_path = 0;
            int disagree = 0, aa = 0;
            for (uint32_t i = 0; i < w * h; ++i) {
                int ca = a[i * 4 + 0], cb = b[i * 4 + 0];
                ink_analytic += ca;
                ink_path += cb;
                /* Classified well away from the boundary: inside vs outside. */
                if (ca > 240 && cb < 15)
                    disagree++;
                if (ca < 15 && cb > 240)
                    disagree++;
                if ((ca > 15 && ca < 240) || (cb > 15 && cb < 240))
                    aa++;
            }
            /* Ink mass agrees to 2.5%. The analytic path carries a small,
             * *systematic* surplus (~1.3%) and that is expected, not a
             * defect: analytic coverage is clamp(0.5 - d, 0, 1), i.e. a
             * one-pixel ramp centred on the boundary, whereas the triangle
             * rasteriser classifies whole samples. The surplus is therefore
             * about (perimeter x 0.25 px), which for this 96x72 shape is
             * ~340 x 0.25 x 255 ~= 21700 of the 1.7M measured total — the
             * numbers agree to a few percent, which is what makes this a
             * check rather than a tolerance chosen to pass. */
            long diff = ink_analytic - ink_path;
            if (diff < 0)
                diff = -diff;
            EXPECT(diff * 1000 <= ink_analytic * 25 + 1000);
            /* No pixel is emphatically on the wrong side: the two paths draw
             * the same shape, not merely a similar area. */
            EXPECT(disagree == 0);
            /* Both paths really did anti-alias something. */
            EXPECT(aa > 100);
        }

        flux_canvas_release(analytic);
        flux_canvas_release(tessellated);
        flux_arena_deinit(&local);
    }
}

/* --- Rotation takes the analytic path (ADR-0118 INV-SDF-03).
 *
 * The SDF pipeline evaluates the field in the shape's own space, so a rotated
 * shape no longer needs an axis-aligned stand-in or a fallback to
 * tessellation. The check mirrors the axis-aligned one: the same rotated shape
 * drawn analytically and by tessellation must agree, with no pixel on the
 * wrong side of the boundary. A shear (not a similarity) must still fall back,
 * so the same comparison is repeated with a shear to prove the analytic path
 * is *not* taking it — there the two only agree because both tessellate. */
static void run_dispatch_case(flux_mat3x2 tx, float curvature, bool expect_analytic) {
    const flux_rect box = {30.0f, 24.0f, 84.0f, 60.0f};
    const float radius = 18.0f;

    flux_canvas *analytic = nullptr;
    flux_canvas *tessellated = nullptr;
    EXPECT(flux_canvas_create_cpu(144, 112, 1.0f, &analytic) == FLUX_OK);
    EXPECT(flux_canvas_create_cpu(144, 112, 1.0f, &tessellated) == FLUX_OK);
    if (!analytic || !tessellated)
        return;

    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    flux_brush brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 255));
    flux_geometry geom = flux_geom_squircle(box, radius, curvature);

    EXPECT(flux_canvas_begin(analytic, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                .clear_color = &clear}) == FLUX_OK);
    flux_canvas_transform(analytic, tx);
    flux_canvas_draw_geometry(analytic, &geom, &brush);
    EXPECT(flux_canvas_end(analytic) == FLUX_OK);

    flux_arena local;
    EXPECT(flux_arena_init(&local, 1u << 16, nullptr) == FLUX_OK);
    flux_path *p = nullptr;
    EXPECT(flux_path_create(&p, &local) == FLUX_OK);
    flux_path_add_squircle(p, box, radius, curvature);
    EXPECT(
        flux_canvas_begin(tessellated, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                .clear_color = &clear}) == FLUX_OK);
    flux_canvas_transform(tessellated, tx);
    flux_canvas_fill_path(tessellated, p, FLUX_FILL_NON_ZERO, &brush);
    EXPECT(flux_canvas_end(tessellated) == FLUX_OK);

    uint32_t w = 0, h = 0;
    const uint8_t *a = flux_canvas_cpu_pixels(analytic, &w, &h, nullptr);
    const uint8_t *b = flux_canvas_cpu_pixels(tessellated, &w, &h, nullptr);
    EXPECT(a != nullptr && b != nullptr);
    if (a && b) {
        long ink_a = 0, ink_b = 0;
        int disagree = 0, aa = 0;
        for (uint32_t i = 0; i < w * h; ++i) {
            int ca = a[i * 4 + 0], cb = b[i * 4 + 0];
            ink_a += ca;
            ink_b += cb;
            if (ca > 240 && cb < 15)
                disagree++;
            if (ca < 15 && cb > 240)
                disagree++;
            if ((ca > 15 && ca < 240) || (cb > 15 && cb < 240))
                aa++;
        }
        EXPECT(ink_a > 0); /* something was drawn at all */
        if (expect_analytic) {
            /* Independently computed shapes: same side of every boundary,
             * and both really did anti-alias. */
            EXPECT(disagree == 0);
            EXPECT(aa > 100);
        } else {
            /* Not a similarity, so the dispatch must keep both drawings on the
             * tessellation path. Identical output is the evidence: an
             * approximated field would differ from the polygon. */
            EXPECT(ink_a == ink_b);
        }
    }

    flux_canvas_release(analytic);
    flux_canvas_release(tessellated);
    flux_arena_deinit(&local);
}

static void test_rotation_and_shear_dispatch(void) {
    /* Rigid rotation about the canvas centre: a similarity, so analytic. */
    flux_mat3x2 rot =
        flux_mat3x2_multiply(flux_mat3x2_translate(72.0f, 56.0f),
                             flux_mat3x2_multiply(flux_mat3x2_rotate(0.5235988f),
                                                  flux_mat3x2_translate(-72.0f, -56.0f)));
    run_dispatch_case(rot, 1.0f, true);
    run_dispatch_case(rot, 0.0f, true);

    /* Uniform scale plus rotation about a different pivot. */
    flux_mat3x2 scale_rot = flux_mat3x2_multiply(
        flux_mat3x2_translate(60.0f, 40.0f),
        flux_mat3x2_multiply(flux_mat3x2_rotate(-0.7853982f),
                             flux_mat3x2_multiply(flux_mat3x2_scale(1.35f, 1.35f),
                                                  flux_mat3x2_translate(-60.0f, -40.0f))));
    run_dispatch_case(scale_rot, 0.6f, true);

    /* Shear is not a similarity: the dispatch must NOT use the field, so the
     * two drawings agree only because both tessellate. The comparison passing
     * here is therefore a non-regression check, not an analytic-coverage
     * claim — which is exactly why `expect_analytic` is false. */
    flux_mat3x2 shear = flux_mat3x2_identity();
    shear.m[2] = 0.35f;
    run_dispatch_case(shear, 1.0f, false);
}

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 1u << 18, nullptr) == FLUX_OK);

    const flux_rect box = {0.0f, 0.0f, 200.0f, 120.0f};
    const flux_point centre = {100.0f, 60.0f};
    const flux_point tr_centre = {200.0f - 32.0f, 32.0f};
    const float radius = 32.0f;
    const float k45_max = 1.189207115f; /* 2^(1/4): L4 diagonal radius */

    /* --- Core claim: the fitted path lies on the field's zero set. --- */
    const float curvatures[] = {0.01f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (unsigned i = 0; i < sizeof(curvatures) / sizeof(curvatures[0]); ++i) {
        float c = curvatures[i];
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_add_squircle(p, box, radius, c);
        uint32_t n = sample_path(p, g_pts, PATH_POINTS);
        EXPECT(n > 100);
        float dev = path_field_deviation(p, centre, box, radius, c);
        EXPECT(dev <= 0.01f * radius);
        flux_arena_reset(&arena);
    }

    /* --- Endpoint continuity: curvature 0 IS the rounded rectangle. --- */
    {
        flux_path *sq = nullptr;
        flux_path *rr = nullptr;
        EXPECT(flux_path_create(&sq, &arena) == FLUX_OK);
        EXPECT(flux_path_create(&rr, &arena) == FLUX_OK);
        flux_path_add_squircle(sq, box, radius, 0.0f);
        flux_path_add_round_rect(rr, box, radius);
        EXPECT(sq->count == rr->count);
        EXPECT(path_field_deviation(sq, centre, box, radius, 0.0f) <= 0.01f * radius);
        flux_arena_reset(&arena);
    }

    /* --- The corner's diagonal bulge equals the field's diagonal radius at
     * every curvature, and grows monotonically. This is the property the OLD
     * path violated: it extended the corner transition with
     * L = r*(1 + 0.52c) instead of reshaping the arc on radius r. --- */
    {
        float prev = -1.0f;
        for (int k = 0; k <= 8; ++k) {
            float c = (float)k / 8.0f;
            if (c < 0.005f)
                c = 0.005f; /* the <= 0.001 case is the round rectangle */
            flux_path *p = nullptr;
            EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
            flux_path_add_squircle(p, box, radius, c);
            float bulge = diagonal_bulge(p, tr_centre);
            float expect = radius * (1.0f + (k45_max - 1.0f) * c);
            EXPECT_NEAR(bulge, expect, radius * 0.01f); /* 1% */
            EXPECT(bulge >= prev - 1e-3f);              /* monotone */
            prev = bulge;
            flux_arena_reset(&arena);
        }
        /* The family ends at the L4 diagonal radius. */
        EXPECT_NEAR(prev, radius * k45_max, radius * 0.01f);
    }

    /* --- The straight edges are untouched by curvature. --- */
    for (int k = 0; k <= 4; ++k) {
        float c = (float)k / 4.0f;
        if (c < 0.005f)
            c = 0.005f;
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_add_squircle(p, box, radius, c);
        uint32_t n = sample_path(p, g_pts, PATH_POINTS);
        bool inside = true;
        float top_y = 1e9f;
        for (uint32_t i = 0; i < n; ++i) {
            /* A cubic fitted to a convex arc can leave the shape's bounding
             * box by a fraction of a pixel (measured: 0.075 px at
             * curvature 1, because one control point sits ~1 px outside).
             * The bound below is generous but finite, so a regression that
             * pushed the corner out by a visible amount would still fail. */
            if (g_pts[i].x < -0.5f || g_pts[i].x > box.w + 0.5f)
                inside = false;
            if (g_pts[i].y < -0.5f || g_pts[i].y > box.h + 0.5f)
                inside = false;
            if (fabsf(g_pts[i].x - box.w * 0.5f) < 0.5f && g_pts[i].y < top_y)
                top_y = g_pts[i].y;
        }
        EXPECT(inside);
        /* The mid-top point is still exactly on the top edge. */
        EXPECT_NEAR(top_y, 0.0f, 1e-3);
        flux_arena_reset(&arena);
    }

    flux_arena_deinit(&arena);

    test_analytic_dispatch_matches_path();
    test_rotation_and_shear_dispatch();
    TEST_SUMMARY();
}
