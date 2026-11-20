/* test_reticle_limits.c — the resource ceilings, and the hostile-input surface.
 *
 * A document arrives from an XDG theme directory, an application's icon set, or
 * a downloaded SVG; none of those is trusted. Every ceiling here is a real
 * allocation bound, and the test asserts that hitting one is a *refusal* (no
 * document) rather than a truncated document that paints half an icon.
 *
 * The cycle/deep-nesting cases double as termination proofs: the parser must
 * finish, not hang, on input designed to make a naive walker loop. */

#include "optics_test.h"

#include <optics/svg.h>

static optics_gfx_result parse(const char *text) {
    optics_svg_document *doc = nullptr;
    optics_gfx_result result = optics_svg_parse(
        &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE, .text = text}, &doc);
    if (result == OPTICS_GFX_OK)
        optics_svg_release(doc);
    else
        OT_EXPECT(doc == nullptr);
    return result;
}

static void inputs_are_bounded(void) {
    OT_EXPECT(parse("") != OPTICS_GFX_OK);
    OT_EXPECT(parse("<svg/>") == OPTICS_GFX_OK);
    /* A lone '>' with no '<' is not a document. */
    OT_EXPECT(parse(">>>>") != OPTICS_GFX_OK);
    /* An unterminated comment must fail, not consume the rest of the buffer as
     * one giant comment and report success. */
    OT_EXPECT(parse("<svg><!-- forever") != OPTICS_GFX_OK);
    OT_EXPECT(parse("<svg><![CDATA[") != OPTICS_GFX_OK);
}

static void shape_ceiling_refuses_the_document(void) {
    /* 8 shapes allowed, 20 supplied. A truncated document would render a
     * plausible-looking icon missing two thirds of its art, which is worse than
     * an error nobody can miss. */
    const char *body = "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>"
                       "<rect width=\"1\" height=\"1\"/><rect width=\"1\" height=\"1\"/>";
    char document[4096];
    snprintf(document, sizeof document, "<svg viewBox=\"0 0 8 8\">%s</svg>", body);

    optics_svg_document *doc = nullptr;
    optics_svg_parse_limits limits = {.max_shapes = 8};
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = document,
                                                        .limits = &limits},
                               &doc) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(doc == nullptr);
    if (doc)
        optics_svg_release(doc);

    /* With the default ceiling the same document is fine. */
    if (optics_svg_parse(
            &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE, .text = document},
            &doc) == OPTICS_GFX_OK) {
        OT_EXPECT(optics_svg_shape_count(doc) == 20);
        optics_svg_release(doc);
    }
}

static void element_ceiling_counts_every_tag(void) {
    const char *document = "<svg viewBox=\"0 0 8 8\"><g><g><g>"
                           "<rect width=\"1\" height=\"1\"/>"
                           "</g></g></g></svg>";
    optics_svg_document *doc = nullptr;
    optics_svg_parse_limits limits = {.max_elements = 3};
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = document,
                                                        .limits = &limits},
                               &doc) != OPTICS_GFX_OK);
    OT_EXPECT(doc == nullptr);
    if (doc)
        optics_svg_release(doc);
}

static void nesting_depth_is_capped(void) {
    /* Group opacity cascades, so nesting is not free; 64 openings against a
     * depth ceiling of 4 must refuse. */
    char document[2048];
    size_t offset = 0;
    offset +=
        (size_t)snprintf(document + offset, sizeof document - offset, "<svg viewBox=\"0 0 8 8\">");
    for (uint32_t i = 0; i < 64 && offset < sizeof document - 64; ++i)
        offset += (size_t)snprintf(document + offset, sizeof document - offset, "<g>");
    offset += (size_t)snprintf(document + offset, sizeof document - offset,
                               "<rect width=\"1\" height=\"1\"/>");
    if (offset < sizeof document - 8)
        offset += (size_t)snprintf(document + offset, sizeof document - offset, "</svg>");
    document[sizeof document - 1] = '\0';

    optics_svg_document *doc = nullptr;
    optics_svg_parse_limits limits = {.max_depth = 4};
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = document,
                                                        .limits = &limits},
                               &doc) != OPTICS_GFX_OK);
    OT_EXPECT(doc == nullptr);
    if (doc)
        optics_svg_release(doc);
}

static void gradient_ceiling_refuses_the_document(void) {
    const char *document =
        "<svg viewBox=\"0 0 8 8\"><defs>"
        "<linearGradient id=\"a\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "<linearGradient id=\"b\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "<linearGradient id=\"c\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "</defs>"
        "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>";
    optics_svg_document *doc = nullptr;
    optics_svg_parse_limits limits = {.max_gradients = 2};
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = document,
                                                        .limits = &limits},
                               &doc) != OPTICS_GFX_OK);
    OT_EXPECT(doc == nullptr);
    if (doc)
        optics_svg_release(doc);
}

static void cyclic_gradient_href_terminates(void) {
    /* a -> b -> a. A naive inheritance walk loops forever; the bounded walk must
     * finish and pick the stops it can reach. */
    optics_svg_document *doc = nullptr;
    optics_gfx_result result = optics_svg_parse(
        &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                 .text = "<svg viewBox=\"0 0 8 8\"><defs>"
                                         "<linearGradient id=\"a\" href=\"#b\"/>"
                                         "<linearGradient id=\"b\" href=\"#a\"/>"
                                         "</defs>"
                                         "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>"},
        &doc);
    OT_EXPECT(result == OPTICS_GFX_OK);
    if (doc) {
        optics_svg_shape_view shape = {};
        if (optics_svg_shape_at(doc, 0, &shape) == OPTICS_GFX_OK) {
            /* No reachable stops: the paint degrades to none, and the shape still
             * exists for a monochrome consumer. */
            OT_EXPECT(shape.fill.kind == OPTICS_SVG_PAINT_NONE);
            OT_EXPECT(shape.cmd_count > 0);
        }
        optics_svg_release(doc);
    }
}

static void self_referencing_gradient_terminates(void) {
    optics_svg_document *doc = nullptr;
    optics_gfx_result result = optics_svg_parse(
        &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                 .text = "<svg viewBox=\"0 0 8 8\"><defs>"
                                         "<linearGradient id=\"a\" href=\"#a\">"
                                         "<stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
                                         "</defs>"
                                         "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>"},
        &doc);
    OT_EXPECT(result == OPTICS_GFX_OK);
    if (doc) {
        optics_svg_shape_view shape = {};
        if (optics_svg_shape_at(doc, 0, &shape) == OPTICS_GFX_OK)
            OT_EXPECT(shape.fill.kind == OPTICS_SVG_PAINT_LINEAR_GRADIENT);
        optics_svg_release(doc);
    }
}

static void malformed_numbers_do_not_poison_geometry(void) {
    /* A shape whose attributes are unparseable contributes no geometry, and must
     * not leave a half-built path for the next shape to inherit. */
    optics_svg_document *doc = nullptr;
    OT_EXPECT(
        optics_svg_parse(
            &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                     .text = "<svg viewBox=\"0 0 8 8\">"
                                             "<rect x=\"abc\" y=\"def\" width=\"1\" height=\"1\"/>"
                                             "</svg>"},
            &doc) == OPTICS_GFX_OK);
    if (doc) {
        /* An unparseable geometry attribute disables the element (SVG error
         * handling); it must not become a shape at the origin. */
        OT_EXPECT(optics_svg_shape_count(doc) == 0);
        optics_svg_release(doc);
    }
}

static void absurd_arc_counts_are_clamped(void) {
    /* A full 360° sweep must decompose to a bounded number of cubics, not to a
     * number proportional to an attacker-chosen magnitude. */
    optics_svg_document *doc = nullptr;
    OT_EXPECT(optics_svg_parse(
                  &(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                           .text = "<svg viewBox=\"0 0 100 100\">"
                                                   "<path d=\"M50 50 A40 40 0 1 0 50 49.9\"/>"
                                                   "</svg>"},
                  &doc) == OPTICS_GFX_OK);
    if (doc) {
        optics_svg_shape_view shape = {};
        if (optics_svg_shape_at(doc, 0, &shape) == OPTICS_GFX_OK) {
            OT_EXPECT(shape.cmd_count > 1);
            OT_EXPECT(shape.cmd_count < 32);
        }
        optics_svg_release(doc);
    }
}

static void zero_length_path_is_not_a_shape(void) {
    optics_svg_document *doc = nullptr;
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = "<svg viewBox=\"0 0 8 8\">"
                                                                "<path d=\"\"/>"
                                                                "</svg>"},
                               &doc) == OPTICS_GFX_OK);
    if (doc) {
        /* An empty `d` is legal and paints nothing, so it yields no shape. */
        OT_EXPECT(optics_svg_shape_count(doc) == 0);
        optics_svg_release(doc);
    }
}

static void command_ceiling_bounds_one_element(void) {
    /* A single `<polyline>` is the amplification case: one element, one
     * attribute, as many commands as it has points. The ceiling must be applied
     * as the commands are produced — checking only after the element completes
     * lets the whole point list be allocated first. */
    char document[8192];
    size_t at = (size_t)snprintf(document, sizeof document,
                                 "<svg viewBox=\"0 0 8 8\">"
                                 "<polyline points=\"");
    for (int i = 0; i < 500; i++)
        at += (size_t)snprintf(document + at, sizeof document - at, "%d,%d ", i, i);
    snprintf(document + at, sizeof document - at, "\"/></svg>");

    optics_svg_document *doc = nullptr;
    optics_svg_parse_limits limits = {.max_commands = 32};
    OT_EXPECT(optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                        .text = document,
                                                        .limits = &limits},
                               &doc) != OPTICS_GFX_OK);
    OT_EXPECT(doc == nullptr);

    /* The same document under a ceiling that fits parses normally, so the
     * refusal above is the limit and not the input. */
    OT_EXPECT(parse(document) == OPTICS_GFX_OK);
}

static void rasterize_refuses_degenerate_requests(void) {
    optics_svg_document *doc = nullptr;
    if (optics_svg_parse(&(optics_svg_parse_opts){.length = OPTICS_SVG_LENGTH_MEASURE,
                                                  .text = "<svg viewBox=\"0 0 8 8\">"
                                                          "<rect width=\"8\" height=\"8\"/></svg>"},
                         &doc) != OPTICS_GFX_OK)
        return;
    optics_svg_pixmap pixmap = {};
    OT_EXPECT(
        optics_svg_rasterize(&(optics_svg_rasterize_opts){.document = doc, .width = 0, .height = 8},
                             &pixmap) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(pixmap.pixels == nullptr);
    OT_EXPECT(
        optics_svg_rasterize(
            &(optics_svg_rasterize_opts){
                .document = doc, .width = 8, .height = 8, .format = (optics_svg_pixel_format)99},
            &pixmap) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(pixmap.pixels == nullptr);
    optics_svg_pixmap_deinit(&pixmap);
    optics_svg_release(doc);
}

int main(void) {
    inputs_are_bounded();
    shape_ceiling_refuses_the_document();
    element_ceiling_counts_every_tag();
    nesting_depth_is_capped();
    gradient_ceiling_refuses_the_document();
    cyclic_gradient_href_terminates();
    self_referencing_gradient_terminates();
    malformed_numbers_do_not_poison_geometry();
    absurd_arc_counts_are_clamped();
    zero_length_path_is_not_a_shape();
    command_ceiling_bounds_one_element();
    rasterize_refuses_degenerate_requests();
    return OT_REPORT();
}
