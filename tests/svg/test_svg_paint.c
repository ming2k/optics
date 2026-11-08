/* test_svg_paint.c — paint resolution: literal colours, currentColor, opacity
 * cascade, gradients and their inheritance.
 *
 * Paint is a *value on the shape*, not a scene-graph node, because that is what
 * lets a consumer recolour after the fact (a monochrome icon tint, a cursor
 * silhouette, a mask bake). These checks pin that contract: the shape carries
 * resolved paint, and `currentColor` survives as a distinct kind rather than
 * being frozen to black at parse time. */

#include "test_helpers.h"

#include <svg/svg.h>

static bool single_shape(const char *text, svg_document **doc, svg_shape_view *shape) {
    *doc = nullptr;
    if (svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = text}, doc) != FLUX_OK)
        return false;
    if (svg_shape_count(*doc) != 1)
        return false;
    return svg_shape_at(*doc, 0, shape) == FLUX_OK;
}

static void hex3_hex6_hex8(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#F00\"/>"
                      "</svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.kind == SVG_PAINT_SOLID);
    /* Packed straight 0xAARRGGBB. */
    CHECK(shape.fill.color == 0xFFFF0000u);
    svg_release(doc);

    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#112233\"/>"
                      "</svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.color == 0xFF112233u);
    svg_release(doc);

    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" "
                      "fill=\"#11223344\"/></svg>",
                      &doc, &shape))
        return;
    CHECK((shape.fill.color >> 24) == 0x44u);
    CHECK((shape.fill.color & 0xFFFFFFu) == 0x112233u);
    svg_release(doc);
}

static void named_colour_and_rgb_function(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" "
                      "fill=\"rebeccapurple\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.color == 0xFF663399u);
    svg_release(doc);

    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" "
                      "fill=\"rgb(10, 20, 30)\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.color == 0xFF0A141Eu);
    svg_release(doc);
}

static void current_color_stays_unresolved(void) {
    /* The defining property of this library's paint model: `currentColor` is the
     * *caller's* colour, so the parser must not bake it. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"none\" "
                      "stroke=\"currentColor\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.stroke.kind == SVG_PAINT_CURRENT_COLOR);
    CHECK(shape.fill.kind == SVG_PAINT_NONE);
    svg_release(doc);
}

static void explicit_color_attribute_resolves_current_color(void) {
    /* `color="#F00"` on an ancestor is what currentColor means; once it is
     * declared, the value is knowable and must be baked. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\" color=\"#00FF00\">"
                      "<rect width=\"8\" height=\"8\" fill=\"currentColor\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.kind == SVG_PAINT_SOLID);
    CHECK(shape.fill.color == 0xFF00FF00u);
    svg_release(doc);
}

static void fill_and_stroke_opacity_are_separate(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#FFFFFF\" "
                      "fill-opacity=\"0.25\" stroke=\"#000000\" stroke-opacity=\"0.5\"/>"
                      "</svg>",
                      &doc, &shape))
        return;
    CHECK_NEAR(shape.fill.opacity, 0.25f, 1e-3);
    CHECK_NEAR(shape.stroke.opacity, 0.5f, 1e-3);
    svg_release(doc);
}

static void element_opacity_cascades_through_groups(void) {
    /* Group opacity multiplies into each descendant. This is the documented
     * boundary (no isolated group compositing); the multiplier itself must still
     * be exact. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><g opacity=\"0.5\">"
                      "<g opacity=\"0.5\"><rect width=\"8\" height=\"8\"/></g></g></svg>",
                      &doc, &shape))
        return;
    CHECK_NEAR(shape.opacity, 0.25f, 1e-3);
    svg_release(doc);
}

static void style_attribute_overrides_presentation_attributes(void) {
    /* CSS cascade order: style="" is author-level and wins over the attribute. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#FF0000\" "
                      "style=\"fill:#0000FF;fill-opacity:0.5\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.color == 0xFF0000FFu);
    CHECK_NEAR(shape.fill.opacity, 0.5f, 1e-3);
    svg_release(doc);
}

static void stroke_topology_defaults_to_svg_values(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><path d=\"M0 0L8 8\" fill=\"none\" "
                      "stroke=\"#000\" stroke-width=\"3\" stroke-linecap=\"round\" "
                      "stroke-linejoin=\"bevel\"/></svg>",
                      &doc, &shape))
        return;
    CHECK_NEAR(shape.stroke_width, 3.0f, 1e-4);
    CHECK(shape.cap == FLUX_CAP_ROUND);
    CHECK(shape.join == FLUX_JOIN_BEVEL);
    svg_release(doc);

    /* Unspecified: butt cap, miter join, width 1 — SVG's initial values, not
     * flux's "0 means fill" convention. */
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><path d=\"M0 0L8 8\" fill=\"none\" "
                      "stroke=\"#000\"/></svg>",
                      &doc, &shape))
        return;
    CHECK_NEAR(shape.stroke_width, 1.0f, 1e-4);
    CHECK(shape.cap == FLUX_CAP_BUTT);
    CHECK(shape.join == FLUX_JOIN_MITER);
    svg_release(doc);
}

static void even_odd_fill_rule_survives(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><path d=\"M0 0H8V8H0Z\" "
                      "fill-rule=\"evenodd\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill_rule == FLUX_FILL_EVEN_ODD);
    svg_release(doc);
}

static void linear_gradient_bakes_into_user_space(void) {
    /* A userSpaceOnUse gradient's endpoints are document coordinates; a consumer
     * must be able to paint it without knowing what a gradient element is. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 100 100\"><defs>"
                      "<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" "
                      "x1=\"0\" y1=\"0\" x2=\"100\" y2=\"0\">"
                      "<stop offset=\"0\" stop-color=\"#FF0000\"/>"
                      "<stop offset=\"1\" stop-color=\"#0000FF\"/>"
                      "</linearGradient></defs>"
                      "<rect width=\"100\" height=\"100\" fill=\"url(#g)\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.kind == SVG_PAINT_LINEAR_GRADIENT);
    svg_gradient_view gradient = {};
    CHECK(svg_gradient_at(doc, shape.fill.gradient, &gradient) == FLUX_OK);
    CHECK(gradient.stop_count == 2);
    CHECK_NEAR(gradient.start.x, 0.0f, 1e-3);
    CHECK_NEAR(gradient.end.x, 100.0f, 1e-3);
    CHECK_NEAR(gradient.stops[0].t, 0.0f, 1e-3);
    CHECK_NEAR(gradient.stops[1].t, 1.0f, 1e-3);
    svg_release(doc);
}

static void object_bounding_box_gradient_scales_to_the_shape(void) {
    /* The default gradientUnits is objectBoundingBox: endpoints are fractions of
     * the shape's own bounds. Baking that is the whole reason bounds are tracked
     * during path parsing. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 100 100\"><defs>"
                      "<linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">"
                      "<stop offset=\"0\" stop-color=\"#FFF\"/>"
                      "<stop offset=\"1\" stop-color=\"#000\"/>"
                      "</linearGradient></defs>"
                      "<rect x=\"20\" y=\"30\" width=\"50\" height=\"10\" fill=\"url(#g)\"/>"
                      "</svg>",
                      &doc, &shape))
        return;
    svg_gradient_view gradient = {};
    CHECK(svg_gradient_at(doc, shape.fill.gradient, &gradient) == FLUX_OK);
    CHECK_NEAR(gradient.start.x, 20.0f, 1e-3);
    CHECK_NEAR(gradient.start.y, 30.0f, 1e-3);
    CHECK_NEAR(gradient.end.x, 70.0f, 1e-3);
    svg_release(doc);
}

static void gradient_inheritance_follows_href(void) {
    /* Stops commonly live on a base gradient and are shared through href. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape(
            "<svg viewBox=\"0 0 100 100\"><defs>"
            "<linearGradient id=\"base\">"
            "<stop offset=\"0\" stop-color=\"#FF0000\"/>"
            "<stop offset=\"1\" stop-color=\"#00FF00\"/>"
            "</linearGradient>"
            "<linearGradient id=\"derived\" href=\"#base\" gradientUnits=\"userSpaceOnUse\" "
            "x1=\"10\" y1=\"0\" x2=\"90\" y2=\"0\"/>"
            "</defs>"
            "<rect width=\"100\" height=\"100\" fill=\"url(#derived)\"/></svg>",
            &doc, &shape))
        return;
    svg_gradient_view gradient = {};
    CHECK(svg_gradient_at(doc, shape.fill.gradient, &gradient) == FLUX_OK);
    CHECK(gradient.stop_count == 2);
    CHECK_NEAR(gradient.start.x, 10.0f, 1e-3);
    CHECK_NEAR(gradient.end.x, 90.0f, 1e-3);
    svg_release(doc);
}

static void radial_gradient_keeps_its_radius(void) {
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 100 100\"><defs>"
                      "<radialGradient id=\"r\" gradientUnits=\"userSpaceOnUse\" "
                      "cx=\"50\" cy=\"50\" r=\"25\">"
                      "<stop offset=\"0\" stop-color=\"#FFF\"/>"
                      "<stop offset=\"1\" stop-color=\"#000\"/>"
                      "</radialGradient></defs>"
                      "<rect width=\"100\" height=\"100\" fill=\"url(#r)\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.kind == SVG_PAINT_RADIAL_GRADIENT);
    svg_gradient_view gradient = {};
    CHECK(svg_gradient_at(doc, shape.fill.gradient, &gradient) == FLUX_OK);
    CHECK_NEAR(gradient.start.x, 50.0f, 1e-3);
    CHECK_NEAR(gradient.radius, 25.0f, 1e-3);
    svg_release(doc);
}

static void dangling_gradient_reference_paints_nothing(void) {
    /* SVG's own answer for `url(#missing)` is "do not render that paint". An
     * invented fallback colour would be a silent visual lie. */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" "
                      "fill=\"url(#nope)\"/></svg>",
                      &doc, &shape))
        return;
    CHECK(shape.fill.kind == SVG_PAINT_NONE);
    /* The geometry still exists, so a monochrome consumer can tint it. */
    CHECK(shape.cmd_count > 0);
    svg_release(doc);
}

static void single_stop_gradient_becomes_flat(void) {
    /* flux interpolates between stops; one stop is SVG's "flat colour". */
    svg_document *doc = nullptr;
    svg_shape_view shape = {};
    if (!single_shape("<svg viewBox=\"0 0 8 8\"><defs>"
                      "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#123456\"/>"
                      "</linearGradient></defs>"
                      "<rect width=\"8\" height=\"8\" fill=\"url(#g)\"/></svg>",
                      &doc, &shape))
        return;
    svg_gradient_view gradient = {};
    CHECK(svg_gradient_at(doc, shape.fill.gradient, &gradient) == FLUX_OK);
    CHECK(gradient.stop_count == 2);
    CHECK(gradient.stops[0].color == gradient.stops[1].color);
    svg_release(doc);
}

static void identical_gradients_share_one_entry(void) {
    /* Two shapes with the same bounds and the same gradient definition must
     * resolve to one interned gradient; otherwise a 400-shape icon set pays 400
     * gradient values for art that used one. */
    svg_document *doc = nullptr;
    if (svg_parse(
            &(svg_parse_opts){
                .length = SVG_LENGTH_MEASURE,
                .text = "<svg viewBox=\"0 0 8 8\"><defs>"
                        "<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\">"
                        "<stop offset=\"0\" stop-color=\"#FFF\"/>"
                        "<stop offset=\"1\" stop-color=\"#000\"/>"
                        "</linearGradient></defs>"
                        "<rect x=\"0\" y=\"0\" width=\"8\" height=\"4\" fill=\"url(#g)\"/>"
                        "<rect x=\"0\" y=\"4\" width=\"8\" height=\"4\" fill=\"url(#g)\"/>"
                        "</svg>"},
            &doc) != FLUX_OK)
        return;
    svg_shape_view a = {}, b = {};
    if (svg_shape_at(doc, 0, &a) == FLUX_OK && svg_shape_at(doc, 1, &b) == FLUX_OK) {
        /* Different bounds with objectBoundingBox units would still differ; this
         * gradient is userSpaceOnUse, so the resolution is identical. */
        CHECK(a.fill.gradient == b.fill.gradient);
    }
    svg_release(doc);
}

int main(void) {
    hex3_hex6_hex8();
    named_colour_and_rgb_function();
    current_color_stays_unresolved();
    explicit_color_attribute_resolves_current_color();
    fill_and_stroke_opacity_are_separate();
    element_opacity_cascades_through_groups();
    style_attribute_overrides_presentation_attributes();
    stroke_topology_defaults_to_svg_values();
    even_odd_fill_rule_survives();
    linear_gradient_bakes_into_user_space();
    object_bounding_box_gradient_scales_to_the_shape();
    gradient_inheritance_follows_href();
    radial_gradient_keeps_its_radius();
    dangling_gradient_reference_paints_nothing();
    single_stop_gradient_becomes_flat();
    identical_gradients_share_one_entry();
    TEST_REPORT();
}
