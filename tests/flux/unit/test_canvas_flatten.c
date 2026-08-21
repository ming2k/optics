/*
 * Curve-flattening tolerance regression.
 *
 * Before this change the flattener used a hard-coded depth limit
 * of 5 (32 segments per curve) regardless of how small or large
 * the curve was on screen. It now subdivides against a screen-pixel
 * tolerance derived from the caller-supplied `pixel_scale`. The
 * test compiles `geometry_flatten.c` into the binary so we can call
 * `flatten_path_to_contours` directly with varying scales.
 */
#include "../../../libs/flux/src/canvas/internal.h"
#include "test_helpers.h"
#include <flux/flux.h>

/* `geometry_flatten.c` uses FLUX_FAIL on scratch exhaustion; not
 * triggered by these tests, but the symbol must still resolve. */
void flux_set_last_error(flux_result code, const char *function, const char *file, int line,
                         const char *message, int32_t backend_code) {
    (void)code;
    (void)function;
    (void)file;
    (void)line;
    (void)message;
    (void)backend_code;
}

static uint32_t flatten_circle(flux_arena *arena, float radius, float pixel_scale, flux_point *pts,
                               uint32_t cap) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return 0;
    flux_path_add_circle(p, 0.0f, 0.0f, radius);

    flux_canvas_contour cons[FLUX_CANVAS_MAX_CONTOURS];
    flatten_multi r =
        flatten_path_to_contours(p, pixel_scale, pts, cap, cons, FLUX_CANVAS_MAX_CONTOURS);
    return r.point_count;
}

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 65536, nullptr) == FLUX_OK);

    flux_point pts[FLUX_CANVAS_PATH_SCRATCH_CAP];

    /* --- screen-pixel tolerance scales subdivision count ---
     * A tiny scale (0.1) corresponds to a curve that occupies very
     * few screen pixels, so the flattener can be coarse. A large
     * scale (50) demands tight subdivision to keep the screen-space
     * deviation under FLATTEN_PIXEL_TOLERANCE. */
    flux_arena_reset(&arena);
    uint32_t low_n = flatten_circle(&arena, 1.0f, 0.1f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP);
    flux_arena_reset(&arena);
    uint32_t mid_n = flatten_circle(&arena, 1.0f, 1.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP);
    flux_arena_reset(&arena);
    uint32_t high_n = flatten_circle(&arena, 1.0f, 50.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP);

    EXPECT(low_n > 0);
    EXPECT(mid_n > low_n); /* tighter tolerance → more points  */
    EXPECT(high_n > mid_n);

    /* --- equivalence: same screen scale, varying object radius ---
     * A 1px circle drawn at 100× and a 100px circle drawn at 1× both
     * cover the same screen extent, so the flattener should produce
     * comparable counts (within a constant factor — flatness depends
     * on relative chord deviation, which scales identically). */
    flux_arena_reset(&arena);
    uint32_t small_high = flatten_circle(&arena, 1.0f, 100.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP);
    flux_arena_reset(&arena);
    uint32_t big_low = flatten_circle(&arena, 100.0f, 1.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP);
    EXPECT(small_high == big_low);

    /* --- mat3x2 scale extraction --- */
    EXPECT_NEAR(flux_canvas_mat3x2_pixel_scale(flux_mat3x2_identity()), 1.0f, 1e-5);
    EXPECT_NEAR(flux_canvas_mat3x2_pixel_scale(flux_mat3x2_scale(3.0f, 3.0f)), 3.0f, 1e-5);
    /* rotation preserves the operator norm (=1 for pure rotation). */
    EXPECT_NEAR(flux_canvas_mat3x2_pixel_scale(flux_mat3x2_rotate(0.7f)), 1.0f, 1e-5);

    /* --- degenerate (all-coincident) cubic/quad emits nothing ---
     * Before the guard, a zero-length cubic recursed to the depth cap
     * and flooded the scratch buffer with 2^16 copies of one point,
     * starving every following contour (an icon's trailing shapes
     * vanished). The guard must emit nothing for the point curve and
     * leave the rest of the path intact. */
    flux_arena_reset(&arena);
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        flux_path_move_to(p, 10.0f, 10.0f);
        flux_path_line_to(p, 20.0f, 10.0f);
        /* all four anchors coincident: a point, not a curve */
        flux_path_cubic_to(p, 20.0f, 10.0f, 20.0f, 10.0f, 20.0f, 10.0f);
        flux_path_line_to(p, 20.0f, 20.0f);
        flux_path_quad_to(p, 20.0f, 20.0f, 20.0f, 20.0f); /* coincident quad */
        flux_path_close(p);
        /* a second subpath after the degenerate verbs must survive */
        flux_path_move_to(p, 40.0f, 40.0f);
        flux_path_line_to(p, 60.0f, 40.0f);

        flux_canvas_contour cons[FLUX_CANVAS_MAX_CONTOURS];
        flatten_multi r = flatten_path_to_contours(p, 1.0f, pts, FLUX_CANVAS_PATH_SCRATCH_CAP, cons,
                                                   FLUX_CANVAS_MAX_CONTOURS);
        EXPECT(r.contour_count == 2);
        EXPECT(r.point_count < 16); /* 2^16-duplicate flood would hit the cap */
        EXPECT(cons[1].count == 2); /* the trailing subpath kept both points */
        flux_point tail = pts[cons[1].start + cons[1].count - 1];
        EXPECT_NEAR(tail.x, 60.0f, 1e-4);
        EXPECT_NEAR(tail.y, 40.0f, 1e-4);
    }

    flux_arena_destroy(&arena);
    TEST_SUMMARY();
}
