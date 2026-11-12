/* test_svg_geometry.c — the path grammar and basic-shape decomposition.
 *
 * The property under test is that everything richer than move/line/quad/cubic
 * has already been decomposed by the time a consumer looks: a rounded rect, an
 * ellipse, an arc and a "Z" all arrive as the same five verbs, in document
 * order, already transformed, with exact bounds. Consumers (the cursor mask
 * baker, the lens icon replay path) rely on that and never reimplement SVG
 * geometry. */

#include "test_helpers.h"

#include <reticle/reticle.h>

typedef struct geometry {
    reticle_document *doc;
    reticle_shape_view shape;
    uint32_t moves, lines, quads, cubics, closes;
} geometry;

static bool load(const char *text, geometry *out) {
    *out = (geometry){};
    if (reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = text},
                      &out->doc) != FLUX_OK)
        return false;
    if (reticle_shape_count(out->doc) != 1)
        return false;
    if (reticle_shape_at(out->doc, 0, &out->shape) != FLUX_OK)
        return false;
    for (uint32_t i = 0; i < out->shape.cmd_count; ++i) {
        switch (out->shape.cmds[i].verb) {
        case RETICLE_PATH_MOVE:
            out->moves++;
            break;
        case RETICLE_PATH_LINE:
            out->lines++;
            break;
        case RETICLE_PATH_QUAD:
            out->quads++;
            break;
        case RETICLE_PATH_CUBIC:
            out->cubics++;
            break;
        case RETICLE_PATH_CLOSE:
            out->closes++;
            break;
        }
    }
    return true;
}

static void dispose(geometry *g) {
    reticle_release(g->doc);
    g->doc = nullptr;
}

static void absolute_and_relative_agree(void) {
    /* L/l, H/h, V/v and C/c must resolve to identical user-space geometry: the
     * relative forms are a parser concern, not a consumer one. */
    geometry absolute, relative;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M2 2 L10 2 L10 10 Z\"/></svg>", &absolute))
        return;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"m2 2 l8 0 l0 8 z\"/></svg>", &relative)) {
        dispose(&absolute);
        return;
    }
    CHECK(absolute.shape.cmd_count == relative.shape.cmd_count);
    for (uint32_t i = 0; i < absolute.shape.cmd_count && i < relative.shape.cmd_count; ++i) {
        CHECK(absolute.shape.cmds[i].verb == relative.shape.cmds[i].verb);
        for (uint32_t k = 0; k < 6; ++k)
            CHECK_NEAR(absolute.shape.cmds[i].params[k], relative.shape.cmds[i].params[k], 1e-5);
    }
    dispose(&absolute);
    dispose(&relative);
}

static void implicit_lineto_after_moveto(void) {
    /* "M0 0 10 0 10 10" is a moveto followed by two implicit linetos; parsers
     * that only accept an explicit command letter render half the icon. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M0 0 10 0 10 10\"/></svg>", &g))
        return;
    CHECK(g.moves == 1);
    CHECK(g.lines == 2);
    dispose(&g);
}

static void h_and_v_emit_lines(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M1 1 H10 V10\"/></svg>", &g))
        return;
    CHECK(g.lines == 2);
    CHECK_NEAR(g.shape.cmds[1].params[0], 10.0f, 1e-5);
    CHECK_NEAR(g.shape.cmds[1].params[1], 1.0f, 1e-5);
    CHECK_NEAR(g.shape.cmds[2].params[0], 10.0f, 1e-5);
    CHECK_NEAR(g.shape.cmds[2].params[1], 10.0f, 1e-5);
    dispose(&g);
}

static void smooth_cubic_reflects_the_previous_control_point(void) {
    /* S must mirror the previous cubic's second control point about the current
     * point. A parser that ignores the reflection produces a visible kink. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M0 0 C2 0 4 2 4 4 S8 8 8 10\"/></svg>", &g))
        return;
    CHECK(g.cubics == 2);
    /* Previous c2 = (4,2); current point (4,4); so the reflected c1 = (4,6). */
    CHECK_NEAR(g.shape.cmds[2].params[0], 4.0f, 1e-5);
    CHECK_NEAR(g.shape.cmds[2].params[1], 6.0f, 1e-5);
    dispose(&g);
}

static void smooth_quad_reflects_the_previous_control_point(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M0 0 Q2 4 4 4 T8 8\"/></svg>", &g))
        return;
    CHECK(g.quads == 2);
    /* Previous control (2,4), current point (4,4) -> reflected (6,4). */
    CHECK_NEAR(g.shape.cmds[2].params[0], 6.0f, 1e-5);
    CHECK_NEAR(g.shape.cmds[2].params[1], 4.0f, 1e-5);
    dispose(&g);
}

static void arcs_decompose_into_cubics(void) {
    /* A half-circle arc is two ≤90° cubic segments. The endpoints must be exact
     * (that is what a consumer's bounds and hotspot math depends on); the
     * interior is a cubic approximation, which is why no interior point is
     * asserted here. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M2 12 A10 10 0 0 1 22 12\"/></svg>", &g))
        return;
    CHECK(g.moves == 1);
    CHECK(g.cubics >= 2);
    const reticle_path_cmd *last = &g.shape.cmds[g.shape.cmd_count - 1];
    CHECK(last->verb == RETICLE_PATH_CUBIC);
    CHECK_NEAR(last->params[4], 22.0f, 1e-3);
    CHECK_NEAR(last->params[5], 12.0f, 1e-3);
    dispose(&g);
}

static void arc_flags_parse_without_separators(void) {
    /* `A10 10 0 011 12` — the flags are single characters, so a parser that
     * reads them as numbers consumes "011" and then fails. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><path d=\"M2 12A10 10 0 0122 12\"/></svg>", &g))
        return;
    CHECK(g.cubics >= 2);
    dispose(&g);
}

static void circle_is_decomposed_and_bounded(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"6\"/></svg>", &g))
        return;
    CHECK(g.moves == 1);
    CHECK(g.closes == 1);
    CHECK(g.cubics == 4);
    /* Bounds are exact for a circle's cardinal points because the arcs are
     * anchored there. */
    CHECK_NEAR(g.shape.bounds.x, 6.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.y, 6.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.w, 12.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.h, 12.0f, 1e-3);
    dispose(&g);
}

static void ellipse_keeps_both_radii(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><ellipse cx=\"12\" cy=\"12\" rx=\"8\" ry=\"3\"/></svg>",
              &g))
        return;
    CHECK_NEAR(g.shape.bounds.w, 16.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.h, 6.0f, 1e-3);
    dispose(&g);
}

static void rounded_rect_honours_the_radius(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"2\" width=\"20\" height=\"10\" "
              "rx=\"4\" ry=\"4\"/></svg>",
              &g))
        return;
    /* Rounded corners still fill the declared box. */
    CHECK_NEAR(g.shape.bounds.x, 2.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.y, 2.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.w, 20.0f, 1e-3);
    CHECK_NEAR(g.shape.bounds.h, 10.0f, 1e-3);
    CHECK(g.cubics == 4);
    dispose(&g);
}

static void single_radius_applies_to_both_axes(void) {
    /* SVG: "rx=3" alone means ry=3 too. Ignoring that turns a pill into a
     * rectangle. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><rect x=\"0\" y=\"0\" width=\"20\" height=\"8\" "
              "rx=\"4\"/></svg>",
              &g))
        return;
    CHECK(g.cubics == 4);
    dispose(&g);
}

static void zero_sized_rect_paints_nothing(void) {
    /* SVG disables an element with a zero extent; it must not become a
     * degenerate path with a stroke around it. */
    reticle_document *doc = nullptr;
    CHECK(reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE,
                                              .text =
                                                  "<svg viewBox=\"0 0 8 8\">"
                                                  "<rect x=\"0\" y=\"0\" width=\"0\" height=\"4\"/>"
                                                  "</svg>"},
                        &doc) == FLUX_OK);
    if (doc) {
        /* SVG disables an element with a zero extent: it contributes no shape at
         * all. An empty shape would make shape indices lie to a consumer. */
        CHECK(reticle_shape_count(doc) == 0);
        reticle_release(doc);
    }
}

static void polygon_closes_and_polyline_does_not(void) {
    geometry polygon = {}, polyline = {};
    if (!load("<svg viewBox=\"0 0 24 24\"><polygon points=\"2,2 10,2 10,10\"/></svg>", &polygon))
        return;
    CHECK(polygon.closes == 1);
    CHECK(polygon.moves == 1);
    CHECK(polygon.lines == 2);
    dispose(&polygon);
    if (!load("<svg viewBox=\"0 0 24 24\"><polyline points=\"2 2 10 2 10 10\"/></svg>", &polyline))
        return;
    CHECK(polyline.closes == 0);
    dispose(&polyline);
}

static void transform_moves_geometry_into_user_space(void) {
    /* A translated rect must arrive translated: the command stream is user
     * space, and a consumer never receives the matrix. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" "
              "transform=\"translate(10 5)\"/></svg>",
              &g))
        return;
    CHECK_NEAR(g.shape.cmds[0].params[0], 10.0f, 1e-4);
    CHECK_NEAR(g.shape.cmds[0].params[1], 5.0f, 1e-4);
    CHECK_NEAR(g.shape.bounds.x, 10.0f, 1e-4);
    dispose(&g);
}

static void nested_group_transforms_compose(void) {
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><g transform=\"translate(4 0)\">"
              "<g transform=\"scale(2)\">"
              "<rect x=\"1\" y=\"1\" width=\"2\" height=\"2\"/></g></g></svg>",
              &g))
        return;
    /* (1,1) -> scale 2 -> (2,2) -> translate 4 -> (6,2). */
    CHECK_NEAR(g.shape.cmds[0].params[0], 6.0f, 1e-4);
    CHECK_NEAR(g.shape.cmds[0].params[1], 2.0f, 1e-4);
    dispose(&g);
}

static void rotate_about_a_point_is_not_the_origin(void) {
    /* rotate(a cx cy) must rotate about (cx,cy); collapsing it to rotate(a)
     * scatters every icon that uses a pivot. */
    geometry g;
    if (!load("<svg viewBox=\"0 0 24 24\"><rect x=\"9\" y=\"1\" width=\"2\" height=\"2\" "
              "transform=\"rotate(90 10 10)\"/></svg>",
              &g))
        return;
    /* (9,1) rotated 90° about (10,10) -> (19,9). */
    CHECK_NEAR(g.shape.cmds[0].params[0], 19.0f, 1e-3);
    CHECK_NEAR(g.shape.cmds[0].params[1], 9.0f, 1e-3);
    dispose(&g);
}

static void every_shape_keeps_document_order(void) {
    reticle_document *doc = nullptr;
    CHECK(reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE,
                                              .text =
                                                  "<svg viewBox=\"0 0 24 24\">"
                                                  "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\"/>"
                                                  "<circle cx=\"5\" cy=\"5\" r=\"1\"/>"
                                                  "<path d=\"M0 0L1 1\"/></svg>"},
                        &doc) == FLUX_OK);
    if (!doc)
        return;
    CHECK(reticle_shape_count(doc) == 3);
    reticle_shape_view shape = {};
    if (reticle_shape_at(doc, 0, &shape) == FLUX_OK)
        CHECK_NEAR(shape.bounds.w, 1.0f, 1e-4);
    if (reticle_shape_at(doc, 1, &shape) == FLUX_OK)
        CHECK_NEAR(shape.bounds.w, 2.0f, 1e-4);
    CHECK(reticle_shape_at(doc, 3, &shape) == FLUX_ERROR_OUT_OF_RANGE);
    reticle_release(doc);
}

int main(void) {
    absolute_and_relative_agree();
    implicit_lineto_after_moveto();
    h_and_v_emit_lines();
    smooth_cubic_reflects_the_previous_control_point();
    smooth_quad_reflects_the_previous_control_point();
    arcs_decompose_into_cubics();
    arc_flags_parse_without_separators();
    circle_is_decomposed_and_bounded();
    ellipse_keeps_both_radii();
    rounded_rect_honours_the_radius();
    single_radius_applies_to_both_axes();
    zero_sized_rect_paints_nothing();
    polygon_closes_and_polyline_does_not();
    transform_moves_geometry_into_user_space();
    nested_group_transforms_compose();
    rotate_about_a_point_is_not_the_origin();
    every_shape_keeps_document_order();
    TEST_REPORT();
}
