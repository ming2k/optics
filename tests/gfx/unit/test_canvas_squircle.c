/*
 * Squircle path ↔ field agreement (ADR-0118 INV-SDF-06).
 *
 * This closes the schism the ADR records: a squircle used to be two different
 * shapes — an Apple-style cubic path in gfx and an L2/L4 superellipse field
 * in material — so the canvas and the materials disagreed for the same
 * `curvature`. Now `optics_gfx_path_add_squircle` emits two cubics per corner fitted
 * to `optics_gfx_sdf_squircle`'s zero set, and this test asserts that every point of
 * the fitted path actually lies on that zero set.
 *
 * The claim is analytic, not golden-image: for a point p on the path,
 * |optics_gfx_sdf_squircle(p − centre, hw, hh, r, c)| is (to within the field's unit
 * gradient) p's distance from the shape the field describes. The measured worst
 * case is 0.12% of the corner radius, and the bound asserted here is 1%.
 *
 * Also pinned: curvature 0 is exactly the rounded rectangle (so introducing
 * curvature never jumps), the corner's diagonal bulge equals the field's
 * diagonal radius at every curvature, and the straight edges are untouched.
 */
#include "optics_test.h"

#include "../../../libs/gfx/src/canvas/internal.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/sdf.h>

#include <math.h>
#include <stdio.h>

#define PATH_POINTS 8192
#define LINE_SUBDIV 8

static optics_gfx_point g_pts[PATH_POINTS];

/* Sample a path's outline, interpolating lines and curves. */
static uint32_t sample_path(const optics_gfx_path *p, optics_gfx_point *out, uint32_t cap) {
    uint32_t n = 0;
    optics_gfx_point cur = {0.0f, 0.0f};
    optics_gfx_point start = {0.0f, 0.0f};
    for (uint32_t i = 0; i < p->count && n < cap; ++i) {
        const optics_gfx_path_segment *s = &p->segments[i];
        switch (s->op) {
        case OPTICS_GFX_PATH_MOVE:
            cur = (optics_gfx_point){s->pts[0], s->pts[1]};
            start = cur;
            out[n++] = cur;
            break;
        case OPTICS_GFX_PATH_LINE: {
            optics_gfx_point e = {s->pts[0], s->pts[1]};
            for (int k = 1; k <= LINE_SUBDIV && n < cap; ++k) {
                float t = (float)k / (float)LINE_SUBDIV;
                out[n++] = (optics_gfx_point){cur.x + (e.x - cur.x) * t, cur.y + (e.y - cur.y) * t};
            }
            cur = e;
            break;
        }
        case OPTICS_GFX_PATH_QUAD: {
            optics_gfx_curve_seg q = optics_gfx_curve_seg_make(
                OPTICS_GFX_CURVE_QUAD, cur, (optics_gfx_point){s->pts[0], s->pts[1]},
                (optics_gfx_point){s->pts[2], s->pts[3]});
            for (int k = 1; k <= 16 && n < cap; ++k)
                out[n++] = optics_gfx_curve_quad_eval(q, (float)k / 16.0f);
            cur = (optics_gfx_point){s->pts[2], s->pts[3]};
            break;
        }
        case OPTICS_GFX_PATH_CUBIC: {
            optics_gfx_point c1 = {s->pts[0], s->pts[1]};
            optics_gfx_point c2 = {s->pts[2], s->pts[3]};
            optics_gfx_point e = {s->pts[4], s->pts[5]};
            for (int k = 1; k <= 16 && n < cap; ++k)
                out[n++] = optics_gfx_curve_cubic_eval(cur, c1, c2, e, (float)k / 16.0f);
            cur = e;
            break;
        }
        case OPTICS_GFX_PATH_CLOSE:
            cur = start;
            break;
        }
    }
    return n;
}

/* Worst |field| over the path's points: how far the drawn outline is from the
 * shape the field describes. */
static float path_field_deviation(const optics_gfx_path *p, optics_gfx_point centre,
                                  optics_gfx_rect r, float radius, float curvature) {
    uint32_t n = sample_path(p, g_pts, PATH_POINTS);
    float hw = r.w * 0.5f, hh = r.h * 0.5f;
    optics_gfx_corner_radii cr = optics_gfx_corner_radii_uniform(radius);
    float worst = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float d =
            optics_gfx_sdf_rrect((optics_gfx_point){g_pts[i].x - centre.x, g_pts[i].y - centre.y},
                                 hw, hh, cr, curvature);
        if (fabsf(d) > worst)
            worst = fabsf(d);
    }
    return worst;
}

/* Distance from the top-right corner's circle centre to the path along the
 * corner diagonal: the field's diagonal radius, in pixels. */
static float diagonal_bulge(const optics_gfx_path *p, optics_gfx_point corner_centre) {
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
 * OPTICS_GFX_GEOM_SQUIRCLE, which now takes the analytic SDF path, and once as the
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
    const optics_gfx_rect box = {24.0f, 20.0f, 96.0f, 72.0f};
    const float radius = 20.0f;
    const float curvatures[] = {0.0f, 0.5f, 1.0f};

    for (unsigned k = 0; k < sizeof(curvatures) / sizeof(curvatures[0]); ++k) {
        float curv = curvatures[k];
        optics_gfx_canvas *analytic = nullptr;
        optics_gfx_canvas *tessellated = nullptr;
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = 144,
                                                .height = 112},
                      &analytic) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = 144,
                                                .height = 112},
                      &tessellated) == OPTICS_GFX_OK);
        if (!analytic || !tessellated)
            return;

        optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
        optics_gfx_brush brush =
            optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 255, 255, 255));
        optics_gfx_geometry geom =
            optics_gfx_geom_rrect(box, optics_gfx_corner_radii_uniform(radius), curv);

        /* Analytic: the geometry takes the SDF pipeline. */
        OT_EXPECT(optics_gfx_canvas_begin(analytic, &(optics_gfx_canvas_pass_desc){
                                                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &clear}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_geometry(
            analytic, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geom, .brush = &brush});
        OT_EXPECT(optics_gfx_canvas_end(analytic) == OPTICS_GFX_OK);

        /* Tessellated: force the path route by drawing a gradient-free but
         * non-solid-equivalent paint? No — draw the path explicitly, which is
         * the independent pipeline. */
        optics_gfx_arena local;
        OT_EXPECT(optics_gfx_arena_init(&local, 1u << 16, nullptr) == OPTICS_GFX_OK);
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &local) == OPTICS_GFX_OK);
        optics_gfx_path_add_rrect(p, box, optics_gfx_corner_radii_uniform(radius), curv);
        OT_EXPECT(optics_gfx_canvas_begin(
                      tessellated,
                      &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                     .clear_color = &clear}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_path(tessellated, p, OPTICS_GFX_FILL_NON_ZERO, &brush);
        OT_EXPECT(optics_gfx_canvas_end(tessellated) == OPTICS_GFX_OK);

        uint32_t w = 0, h = 0;
        const uint8_t *a = optics_gfx_canvas_cpu_pixels(analytic, &w, &h, nullptr);
        const uint8_t *b = optics_gfx_canvas_cpu_pixels(tessellated, &w, &h, nullptr);
        OT_EXPECT(a != nullptr && b != nullptr);
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
            OT_EXPECT(diff * 1000 <= ink_analytic * 25 + 1000);
            /* No pixel is emphatically on the wrong side: the two paths draw
             * the same shape, not merely a similar area. */
            OT_EXPECT(disagree == 0);
            /* Both paths really did anti-alias something. */
            OT_EXPECT(aa > 100);
        }

        optics_gfx_canvas_release(analytic);
        optics_gfx_canvas_release(tessellated);
        optics_gfx_arena_deinit(&local);
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
static void run_dispatch_case(optics_gfx_mat3x2 tx, float curvature, bool expect_analytic) {
    const optics_gfx_rect box = {30.0f, 24.0f, 84.0f, 60.0f};
    const float radius = 18.0f;

    optics_gfx_canvas *analytic = nullptr;
    optics_gfx_canvas *tessellated = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 144,
                                                           .height = 112},
                                 &analytic) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 144,
                                                           .height = 112},
                                 &tessellated) == OPTICS_GFX_OK);
    if (!analytic || !tessellated)
        return;

    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    optics_gfx_brush brush =
        optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 255, 255, 255));
    optics_gfx_geometry geom =
        optics_gfx_geom_rrect(box, optics_gfx_corner_radii_uniform(radius), curvature);

    OT_EXPECT(optics_gfx_canvas_begin(analytic, &(optics_gfx_canvas_pass_desc){
                                                    .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_transform(analytic, tx);
    optics_gfx_canvas_draw_geometry(
        analytic, &(optics_gfx_canvas_draw_geometry_opts){.geom = &geom, .brush = &brush});
    OT_EXPECT(optics_gfx_canvas_end(analytic) == OPTICS_GFX_OK);

    optics_gfx_arena local;
    OT_EXPECT(optics_gfx_arena_init(&local, 1u << 16, nullptr) == OPTICS_GFX_OK);
    optics_gfx_path *p = nullptr;
    OT_EXPECT(optics_gfx_path_create(&p, &local) == OPTICS_GFX_OK);
    optics_gfx_path_add_rrect(p, box, optics_gfx_corner_radii_uniform(radius), curvature);
    OT_EXPECT(optics_gfx_canvas_begin(tessellated, &(optics_gfx_canvas_pass_desc){
                                                       .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                       .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_transform(tessellated, tx);
    optics_gfx_canvas_fill_path(tessellated, p, OPTICS_GFX_FILL_NON_ZERO, &brush);
    OT_EXPECT(optics_gfx_canvas_end(tessellated) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0;
    const uint8_t *a = optics_gfx_canvas_cpu_pixels(analytic, &w, &h, nullptr);
    const uint8_t *b = optics_gfx_canvas_cpu_pixels(tessellated, &w, &h, nullptr);
    OT_EXPECT(a != nullptr && b != nullptr);
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
        OT_EXPECT(ink_a > 0); /* something was drawn at all */
        if (expect_analytic) {
            /* Independently computed shapes: same side of every boundary,
             * and both really did anti-alias. */
            OT_EXPECT(disagree == 0);
            OT_EXPECT(aa > 100);
        } else {
            /* Not a similarity, so the dispatch must keep both drawings on the
             * tessellation path. Identical output is the evidence: an
             * approximated field would differ from the polygon. */
            OT_EXPECT(ink_a == ink_b);
        }
    }

    optics_gfx_canvas_release(analytic);
    optics_gfx_canvas_release(tessellated);
    optics_gfx_arena_deinit(&local);
}

static void test_rotation_and_shear_dispatch(void) {
    /* Rigid rotation about the canvas centre: a similarity, so analytic. */
    optics_gfx_mat3x2 rot = optics_gfx_mat3x2_multiply(
        optics_gfx_mat3x2_translate(72.0f, 56.0f),
        optics_gfx_mat3x2_multiply(optics_gfx_mat3x2_rotate(0.5235988f),
                                   optics_gfx_mat3x2_translate(-72.0f, -56.0f)));
    run_dispatch_case(rot, 1.0f, true);
    run_dispatch_case(rot, 0.0f, true);

    /* Uniform scale plus rotation about a different pivot. */
    optics_gfx_mat3x2 scale_rot = optics_gfx_mat3x2_multiply(
        optics_gfx_mat3x2_translate(60.0f, 40.0f),
        optics_gfx_mat3x2_multiply(
            optics_gfx_mat3x2_rotate(-0.7853982f),
            optics_gfx_mat3x2_multiply(optics_gfx_mat3x2_scale(1.35f, 1.35f),
                                       optics_gfx_mat3x2_translate(-60.0f, -40.0f))));
    run_dispatch_case(scale_rot, 0.6f, true);

    /* Shear is not a similarity: the dispatch must NOT use the field, so the
     * two drawings agree only because both tessellate. The comparison passing
     * here is therefore a non-regression check, not an analytic-coverage
     * claim — which is exactly why `expect_analytic` is false. */
    optics_gfx_mat3x2 shear = optics_gfx_mat3x2_identity();
    shear.m[2] = 0.35f;
    run_dispatch_case(shear, 1.0f, false);
}

int main(void) {
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 1u << 18, nullptr) == OPTICS_GFX_OK);

    const optics_gfx_rect box = {0.0f, 0.0f, 200.0f, 120.0f};
    const optics_gfx_point centre = {100.0f, 60.0f};
    const optics_gfx_point tr_centre = {200.0f - 32.0f, 32.0f};
    const float radius = 32.0f;
    const float k45_max = 1.189207115f; /* 2^(1/4): L4 diagonal radius */

    /* --- Core claim: the fitted path lies on the field's zero set. --- */
    const float curvatures[] = {0.01f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (unsigned i = 0; i < sizeof(curvatures) / sizeof(curvatures[0]); ++i) {
        float c = curvatures[i];
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_rrect(p, box, optics_gfx_corner_radii_uniform(radius), c);
        uint32_t n = sample_path(p, g_pts, PATH_POINTS);
        OT_EXPECT(n > 100);
        float dev = path_field_deviation(p, centre, box, radius, c);
        OT_EXPECT(dev <= 0.01f * radius);
        optics_gfx_arena_reset(&arena);
    }

    /* --- Endpoint continuity: curvature 0 IS the circular rounded rectangle. --- */
    {
        optics_gfx_path *rr = nullptr;
        OT_EXPECT(optics_gfx_path_create(&rr, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_rrect(rr, box, optics_gfx_corner_radii_uniform(radius), 0.0f);
        OT_EXPECT(path_field_deviation(rr, centre, box, radius, 0.0f) <= 0.01f * radius);
        optics_gfx_arena_reset(&arena);
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
            optics_gfx_path *p = nullptr;
            OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
            optics_gfx_path_add_rrect(p, box, optics_gfx_corner_radii_uniform(radius), c);
            float bulge = diagonal_bulge(p, tr_centre);
            float expect = radius * (1.0f + (k45_max - 1.0f) * c);
            OT_EXPECT_NEAR(bulge, expect, radius * 0.01f); /* 1% */
            OT_EXPECT(bulge >= prev - 1e-3f);              /* monotone */
            prev = bulge;
            optics_gfx_arena_reset(&arena);
        }
        /* The family ends at the L4 diagonal radius. */
        OT_EXPECT_NEAR(prev, radius * k45_max, radius * 0.01f);
    }

    /* --- The straight edges are untouched by curvature. --- */
    for (int k = 0; k <= 4; ++k) {
        float c = (float)k / 4.0f;
        if (c < 0.005f)
            c = 0.005f;
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_rrect(p, box, optics_gfx_corner_radii_uniform(radius), c);
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
        OT_EXPECT(inside);
        /* The mid-top point is still exactly on the top edge. */
        OT_EXPECT_NEAR(top_y, 0.0f, 1e-3);
        optics_gfx_arena_reset(&arena);
    }

    optics_gfx_arena_deinit(&arena);

    test_analytic_dispatch_matches_path();
    test_rotation_and_shear_dispatch();

    /* Per-corner analytic SDF ↔ fitted path agreement (ADR-0175): quarter-arc
     * [0, 0, 22, 0], bezel-flush tab [0, 0, 8, 8], and asymmetric 4-corner
     * squircle [4, 14, 24, 0]. Both zero-set deviation and analytic-vs-path
     * rasterisation parity must hold with zero wrong-side pixels. */
    {
        static const struct {
            optics_gfx_rect box;
            optics_gfx_corner_radii radii;
            float curv;
        } cases[] = {
            {{0.0f, 0.0f, 22.0f, 22.0f}, {0.0f, 0.0f, 22.0f, 0.0f}, 0.0f},
            {{0.0f, 0.0f, 22.0f, 22.0f}, {0.0f, 0.0f, 22.0f, 0.0f}, 0.6f},
            {{0.0f, 0.0f, 22.0f, 22.0f}, {0.0f, 0.0f, 22.0f, 0.0f}, 1.0f},
            {{8.0f, 0.0f, 64.0f, 8.0f}, {0.0f, 0.0f, 8.0f, 8.0f}, 0.6f},
            {{4.0f, 4.0f, 80.0f, 50.0f}, {4.0f, 14.0f, 24.0f, 0.0f}, 0.7f},
            {{10.0f, 10.0f, 22.0f, 22.0f}, {22.0f, 0.0f, 22.0f, 0.0f}, 0.5f},
        };
        optics_gfx_arena local_arena;
        OT_EXPECT(optics_gfx_arena_init(&local_arena, 65536, nullptr) == OPTICS_GFX_OK);
        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
            optics_gfx_path *p = nullptr;
            OT_EXPECT(optics_gfx_path_create(&p, &local_arena) == OPTICS_GFX_OK);
            optics_gfx_path_add_rrect(p, cases[ci].box, cases[ci].radii, cases[ci].curv);
            uint32_t n = sample_path(p, g_pts, PATH_POINTS);
            OT_EXPECT(n > 8);
            float cx = cases[ci].box.x + cases[ci].box.w * 0.5f;
            float cy = cases[ci].box.y + cases[ci].box.h * 0.5f;
            float hx = cases[ci].box.w * 0.5f;
            float hy = cases[ci].box.h * 0.5f;
            float worst = 0.0f;
            for (uint32_t i = 0; i < n; ++i) {
                optics_gfx_point rel = {g_pts[i].x - cx, g_pts[i].y - cy};
                float d = fabsf(optics_gfx_sdf_rrect(rel, hx, hy, cases[ci].radii, cases[ci].curv));
                if (d > worst)
                    worst = d;
            }
            OT_EXPECT(worst <= 0.25f);
            optics_gfx_arena_reset(&local_arena);
        }

        /* Raster parity: analytic SDF vs tessellated path on CPU canvas. */
        optics_gfx_canvas *c_sdf = nullptr, *c_path = nullptr;
        optics_gfx_canvas_desc desc = {.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                       .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                       .scale = 1,
                                       .width = 96,
                                       .height = 64};
        OT_EXPECT(optics_gfx_canvas_create(&desc, &c_sdf) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_create(&desc, &c_path) == OPTICS_GFX_OK);
        optics_gfx_color clear = 0;
        optics_gfx_brush white = optics_gfx_brush_solid(0xFFFFFFFFu);
        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
            optics_gfx_geometry g_sdf =
                optics_gfx_geom_rrect(cases[ci].box, cases[ci].radii, cases[ci].curv);
            optics_gfx_path *p = nullptr;
            OT_EXPECT(optics_gfx_path_create(&p, &local_arena) == OPTICS_GFX_OK);
            optics_gfx_path_add_rrect(p, cases[ci].box, cases[ci].radii, cases[ci].curv);
            optics_gfx_geometry g_path = optics_gfx_geom_path(p);

            OT_EXPECT(optics_gfx_canvas_begin(c_sdf, &(optics_gfx_canvas_pass_desc){
                                                         .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == OPTICS_GFX_OK);
            optics_gfx_canvas_draw_geometry(
                c_sdf, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g_sdf, .brush = &white});
            OT_EXPECT(optics_gfx_canvas_end(c_sdf) == OPTICS_GFX_OK);

            OT_EXPECT(optics_gfx_canvas_begin(c_path, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
            optics_gfx_canvas_draw_geometry(
                c_path, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g_path, .brush = &white});
            OT_EXPECT(optics_gfx_canvas_end(c_path) == OPTICS_GFX_OK);

            uint32_t s_a = 0, s_b = 0;
            const uint8_t *pa = optics_gfx_canvas_read_pixels(c_sdf, nullptr, nullptr, &s_a);
            const uint8_t *pb = optics_gfx_canvas_read_pixels(c_path, nullptr, nullptr, &s_b);
            uint32_t wrong_side = 0;
            for (uint32_t y = 0; y < 64; ++y) {
                for (uint32_t x = 0; x < 96; ++x) {
                    uint8_t aa = pa[y * s_a + x * 4 + 3];
                    uint8_t ab = pb[y * s_b + x * 4 + 3];
                    if ((aa == 255 && ab == 0) || (aa == 0 && ab == 255))
                        wrong_side++;
                }
            }
            OT_EXPECT(wrong_side == 0);
            optics_gfx_arena_reset(&local_arena);
        }
        optics_gfx_canvas_release(c_sdf);
        optics_gfx_canvas_release(c_path);
        optics_gfx_arena_deinit(&local_arena);
    }

    return OT_REPORT();
}
