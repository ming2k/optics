/*
 * Curve-flattening tolerance regression.
 *
 * The flattener subdivides against a screen-pixel tolerance derived from the
 * caller-supplied `pixel_scale`, now via the curve kernel (ADR-0115):
 * each cubic is reduced to line/quadratic pieces with a bounded deviation and
 * each piece is sampled at its Wang segment count. The test compiles
 * `curve_flatten.c` into the binary so we can call `flatten_path_to_contours`
 * directly with varying scales.
 */
#include "../../../libs/gfx/src/canvas/internal.h"
#include "optics_test.h"
#include <optics/gfx.h>

/* `curve_flatten.c` (and the kernel it calls) use OPTICS_GFX_FAIL on scratch
 * exhaustion; not triggered by these tests, but the symbol must still
 * resolve. */
void optics_gfx_set_last_error(optics_gfx_result code, const char *function, const char *file,
                               int line, const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

static uint32_t flatten_circle(optics_gfx_arena *arena, float radius, float pixel_scale,
                               optics_gfx_point *pts, uint32_t cap) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return 0;
    optics_gfx_path_add_circle(p, 0.0f, 0.0f, radius);

    optics_gfx_canvas_contour cons[OPTICS_GFX_CANVAS_MAX_CONTOURS];
    flatten_multi r =
        flatten_path_to_contours(p, pixel_scale, pts, cap, cons, OPTICS_GFX_CANVAS_MAX_CONTOURS);
    return r.point_count;
}

#include <stdlib.h>

typedef struct dyn_buf {
    optics_gfx_point *pts;
    uint32_t pts_cap;
    optics_gfx_canvas_contour *cons;
    uint32_t cons_cap;
} dyn_buf;

static bool test_dyn_grow(void *ctx, uint32_t need_pts, uint32_t need_cons, optics_gfx_point **pts,
                          uint32_t *pts_cap, optics_gfx_canvas_contour **cons, uint32_t *cons_cap) {
    dyn_buf *b = ctx;
    if (need_pts > b->pts_cap) {
        uint32_t cap = b->pts_cap ? b->pts_cap * 2u : 64u;
        while (cap < need_pts)
            cap *= 2u;
        optics_gfx_point *np = realloc(b->pts, (size_t)cap * sizeof(*np));
        if (!np)
            return false;
        b->pts = np;
        b->pts_cap = cap;
    }
    if (need_cons > b->cons_cap) {
        uint32_t cap = b->cons_cap ? b->cons_cap * 2u : 16u;
        while (cap < need_cons)
            cap *= 2u;
        optics_gfx_canvas_contour *nc = realloc(b->cons, (size_t)cap * sizeof(*nc));
        if (!nc)
            return false;
        b->cons = nc;
        b->cons_cap = cap;
    }
    *pts = b->pts;
    *pts_cap = b->pts_cap;
    *cons = b->cons;
    *cons_cap = b->cons_cap;
    return true;
}

int main(void) {
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 65536, nullptr) == OPTICS_GFX_OK);

    optics_gfx_point pts[OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP];

    /* --- screen-pixel tolerance scales subdivision count ---
     * A tiny scale (0.1) corresponds to a curve that occupies very
     * few screen pixels, so the flattener can be coarse. A large
     * scale (50) demands tight subdivision to keep the screen-space
     * deviation under FLATTEN_PIXEL_TOLERANCE. */
    optics_gfx_arena_reset(&arena);
    uint32_t low_n = flatten_circle(&arena, 1.0f, 0.1f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);
    optics_gfx_arena_reset(&arena);
    uint32_t mid_n = flatten_circle(&arena, 1.0f, 1.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);
    optics_gfx_arena_reset(&arena);
    uint32_t high_n = flatten_circle(&arena, 1.0f, 50.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);

    OT_EXPECT(low_n > 0);
    OT_EXPECT(mid_n > low_n); /* tighter tolerance → more points  */
    OT_EXPECT(high_n > mid_n);

    /* --- equivalence: same screen scale, varying object radius ---
     * A 1px circle drawn at 100× and a 100px circle drawn at 1× both
     * cover the same screen extent, so the flattener should produce
     * comparable counts (within a constant factor — flatness depends
     * on relative chord deviation, which scales identically). */
    optics_gfx_arena_reset(&arena);
    uint32_t small_high =
        flatten_circle(&arena, 1.0f, 100.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);
    optics_gfx_arena_reset(&arena);
    uint32_t big_low =
        flatten_circle(&arena, 100.0f, 1.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);
    OT_EXPECT(small_high == big_low);

    /* --- mat3x2 scale extraction --- */
    OT_EXPECT_NEAR(optics_gfx_canvas_mat3x2_pixel_scale(optics_gfx_mat3x2_identity()), 1.0f, 1e-5);
    OT_EXPECT_NEAR(optics_gfx_canvas_mat3x2_pixel_scale(optics_gfx_mat3x2_scale(3.0f, 3.0f)), 3.0f,
                   1e-5);
    /* rotation preserves the operator norm (=1 for pure rotation). */
    OT_EXPECT_NEAR(optics_gfx_canvas_mat3x2_pixel_scale(optics_gfx_mat3x2_rotate(0.7f)), 1.0f,
                   1e-5);

    /* --- degenerate (all-coincident) cubic/quad emits nothing ---
     * Before the guard, a zero-length cubic recursed to the depth cap
     * and flooded the scratch buffer with 2^16 copies of one point,
     * starving every following contour (an icon's trailing shapes
     * vanished). The guard must emit nothing for the point curve and
     * leave the rest of the path intact. */
    optics_gfx_arena_reset(&arena);
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_move_to(p, 10.0f, 10.0f);
        optics_gfx_path_line_to(p, 20.0f, 10.0f);
        /* all four anchors coincident: a point, not a curve */
        optics_gfx_path_cubic_to(p, 20.0f, 10.0f, 20.0f, 10.0f, 20.0f, 10.0f);
        optics_gfx_path_line_to(p, 20.0f, 20.0f);
        optics_gfx_path_quad_to(p, 20.0f, 20.0f, 20.0f, 20.0f); /* coincident quad */
        optics_gfx_path_close(p);
        /* a second subpath after the degenerate verbs must survive */
        optics_gfx_path_move_to(p, 40.0f, 40.0f);
        optics_gfx_path_line_to(p, 60.0f, 40.0f);

        optics_gfx_canvas_contour cons[OPTICS_GFX_CANVAS_MAX_CONTOURS];
        flatten_multi r = flatten_path_to_contours(p, 1.0f, pts, OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP,
                                                   cons, OPTICS_GFX_CANVAS_MAX_CONTOURS);
        OT_EXPECT(r.contour_count == 2);
        OT_EXPECT(r.point_count < 16); /* 2^16-duplicate flood would hit the cap */
        OT_EXPECT(cons[1].count == 2); /* the trailing subpath kept both points */
        optics_gfx_point tail = pts[cons[1].start + cons[1].count - 1];
        OT_EXPECT_NEAR(tail.x, 60.0f, 1e-4);
        OT_EXPECT_NEAR(tail.y, 40.0f, 1e-4);
    }

    /* --- dynamic growth beyond 2048 points and 64 contours ---------------
     * Previously OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP (2048) and
     * OPTICS_GFX_CANVAS_MAX_CONTOURS (64) silently dropped trailing geometry.
     * Dynamic flattening preserves every point and contour. */
    optics_gfx_arena_reset(&arena);
    {
        optics_gfx_arena big_arena;
        OT_EXPECT(optics_gfx_arena_init(&big_arena, 524288, nullptr) == OPTICS_GFX_OK);
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &big_arena) == OPTICS_GFX_OK);
        for (uint32_t c = 0; c < 80; ++c) {
            float y = (float)c * 2.0f;
            optics_gfx_path_move_to(p, 0.0f, y);
            for (uint32_t s = 1; s <= 40; ++s)
                optics_gfx_path_line_to(p, (float)s * 3.0f, y + ((s & 1u) ? 1.0f : 0.0f));
            optics_gfx_path_close(p);
        }
        OT_EXPECT(optics_gfx_path_dropped_count(p) == 0);
        dyn_buf db = {
            .pts = malloc(128 * sizeof(optics_gfx_point)),
            .pts_cap = 128,
            .cons = malloc(16 * sizeof(optics_gfx_canvas_contour)),
            .cons_cap = 16,
        };
        flatten_multi fm = flatten_path_to_contours_dyn(p, 1.0f, db.pts, db.pts_cap, db.cons,
                                                        db.cons_cap, test_dyn_grow, &db);
        OT_EXPECT(fm.contour_count == 80);
        OT_EXPECT(fm.point_count > 3000);
        OT_EXPECT(fm.point_count > OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP);
        OT_EXPECT(fm.contour_count > OPTICS_GFX_CANVAS_MAX_CONTOURS);
        if (fm.contour_count == 80) {
            optics_gfx_point last_tail = db.pts[db.cons[79].start + db.cons[79].count - 1];
            OT_EXPECT_NEAR(last_tail.x, 0.0f, 1e-4);
            OT_EXPECT_NEAR(last_tail.y, 158.0f, 1e-4);
        }
        free(db.pts);
        free(db.cons);
        optics_gfx_arena_deinit(&big_arena);
    }

    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
