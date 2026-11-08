/* test_svg_limits.c — the resource ceilings, and the hostile-input surface.
 *
 * A document arrives from an XDG theme directory, an application's icon set, or
 * a downloaded SVG; none of those is trusted. Every ceiling here is a real
 * allocation bound, and the test asserts that hitting one is a *refusal* (no
 * document) rather than a truncated document that paints half an icon.
 *
 * The cycle/deep-nesting cases double as termination proofs: the parser must
 * finish, not hang, on input designed to make a naive walker loop. */

#include "test_helpers.h"

#include <svg/svg.h>

static flux_result parse(const char *text) {
    svg_document *doc = nullptr;
    flux_result result =
        svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = text}, &doc);
    if (result == FLUX_OK)
        svg_release(doc);
    else
        CHECK(doc == nullptr);
    return result;
}

static void inputs_are_bounded(void) {
    CHECK(parse("") != FLUX_OK);
    CHECK(parse("<svg/>") == FLUX_OK);
    /* A lone '>' with no '<' is not a document. */
    CHECK(parse(">>>>") != FLUX_OK);
    /* An unterminated comment must fail, not consume the rest of the buffer as
     * one giant comment and report success. */
    CHECK(parse("<svg><!-- forever") != FLUX_OK);
    CHECK(parse("<svg><![CDATA[") != FLUX_OK);
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

    svg_document *doc = nullptr;
    svg_parse_limits limits = {.max_shapes = 8};
    CHECK(svg_parse(
              &(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document, .limits = &limits},
              &doc) == FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(doc == nullptr);
    if (doc)
        svg_release(doc);

    /* With the default ceiling the same document is fine. */
    if (svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document}, &doc) ==
        FLUX_OK) {
        CHECK(svg_shape_count(doc) == 20);
        svg_release(doc);
    }
}

static void element_ceiling_counts_every_tag(void) {
    const char *document = "<svg viewBox=\"0 0 8 8\"><g><g><g>"
                           "<rect width=\"1\" height=\"1\"/>"
                           "</g></g></g></svg>";
    svg_document *doc = nullptr;
    svg_parse_limits limits = {.max_elements = 3};
    CHECK(svg_parse(
              &(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document, .limits = &limits},
              &doc) != FLUX_OK);
    CHECK(doc == nullptr);
    if (doc)
        svg_release(doc);
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

    svg_document *doc = nullptr;
    svg_parse_limits limits = {.max_depth = 4};
    CHECK(svg_parse(
              &(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document, .limits = &limits},
              &doc) != FLUX_OK);
    CHECK(doc == nullptr);
    if (doc)
        svg_release(doc);
}

static void gradient_ceiling_refuses_the_document(void) {
    const char *document =
        "<svg viewBox=\"0 0 8 8\"><defs>"
        "<linearGradient id=\"a\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "<linearGradient id=\"b\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "<linearGradient id=\"c\"><stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
        "</defs>"
        "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>";
    svg_document *doc = nullptr;
    svg_parse_limits limits = {.max_gradients = 2};
    CHECK(svg_parse(
              &(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document, .limits = &limits},
              &doc) != FLUX_OK);
    CHECK(doc == nullptr);
    if (doc)
        svg_release(doc);
}

static void cyclic_gradient_href_terminates(void) {
    /* a -> b -> a. A naive inheritance walk loops forever; the bounded walk must
     * finish and pick the stops it can reach. */
    svg_document *doc = nullptr;
    flux_result result = svg_parse(
        &(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                          .text = "<svg viewBox=\"0 0 8 8\"><defs>"
                                  "<linearGradient id=\"a\" href=\"#b\"/>"
                                  "<linearGradient id=\"b\" href=\"#a\"/>"
                                  "</defs>"
                                  "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>"},
        &doc);
    CHECK(result == FLUX_OK);
    if (doc) {
        svg_shape_view shape = {};
        if (svg_shape_at(doc, 0, &shape) == FLUX_OK) {
            /* No reachable stops: the paint degrades to none, and the shape still
             * exists for a monochrome consumer. */
            CHECK(shape.fill.kind == SVG_PAINT_NONE);
            CHECK(shape.cmd_count > 0);
        }
        svg_release(doc);
    }
}

static void self_referencing_gradient_terminates(void) {
    svg_document *doc = nullptr;
    flux_result result = svg_parse(
        &(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                          .text = "<svg viewBox=\"0 0 8 8\"><defs>"
                                  "<linearGradient id=\"a\" href=\"#a\">"
                                  "<stop offset=\"0\" stop-color=\"#FFF\"/></linearGradient>"
                                  "</defs>"
                                  "<rect width=\"8\" height=\"8\" fill=\"url(#a)\"/></svg>"},
        &doc);
    CHECK(result == FLUX_OK);
    if (doc) {
        svg_shape_view shape = {};
        if (svg_shape_at(doc, 0, &shape) == FLUX_OK)
            CHECK(shape.fill.kind == SVG_PAINT_LINEAR_GRADIENT);
        svg_release(doc);
    }
}

static void malformed_numbers_do_not_poison_geometry(void) {
    /* A shape whose attributes are unparseable contributes no geometry, and must
     * not leave a half-built path for the next shape to inherit. */
    svg_document *doc = nullptr;
    CHECK(svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                                      .text = "<svg viewBox=\"0 0 8 8\">"
                                              "<rect x=\"abc\" y=\"def\" width=\"1\" height=\"1\"/>"
                                              "</svg>"},
                    &doc) == FLUX_OK);
    if (doc) {
        /* An unparseable geometry attribute disables the element (SVG error
         * handling); it must not become a shape at the origin. */
        CHECK(svg_shape_count(doc) == 0);
        svg_release(doc);
    }
}

static void absurd_arc_counts_are_clamped(void) {
    /* A full 360° sweep must decompose to a bounded number of cubics, not to a
     * number proportional to an attacker-chosen magnitude. */
    svg_document *doc = nullptr;
    CHECK(svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                                      .text = "<svg viewBox=\"0 0 100 100\">"
                                              "<path d=\"M50 50 A40 40 0 1 0 50 49.9\"/>"
                                              "</svg>"},
                    &doc) == FLUX_OK);
    if (doc) {
        svg_shape_view shape = {};
        if (svg_shape_at(doc, 0, &shape) == FLUX_OK) {
            CHECK(shape.cmd_count > 1);
            CHECK(shape.cmd_count < 32);
        }
        svg_release(doc);
    }
}

static void zero_length_path_is_not_a_shape(void) {
    svg_document *doc = nullptr;
    CHECK(svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                                      .text = "<svg viewBox=\"0 0 8 8\">"
                                              "<path d=\"\"/>"
                                              "</svg>"},
                    &doc) == FLUX_OK);
    if (doc) {
        /* An empty `d` is legal and paints nothing, so it yields no shape. */
        CHECK(svg_shape_count(doc) == 0);
        svg_release(doc);
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

    svg_document *doc = nullptr;
    svg_parse_limits limits = {.max_commands = 32};
    CHECK(svg_parse(
              &(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = document, .limits = &limits},
              &doc) != FLUX_OK);
    CHECK(doc == nullptr);

    /* The same document under a ceiling that fits parses normally, so the
     * refusal above is the limit and not the input. */
    CHECK(parse(document) == FLUX_OK);
}

static void rasterize_refuses_degenerate_requests(void) {
    svg_document *doc = nullptr;
    if (svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE,
                                    .text = "<svg viewBox=\"0 0 8 8\">"
                                            "<rect width=\"8\" height=\"8\"/></svg>"},
                  &doc) != FLUX_OK)
        return;
    svg_pixmap pixmap = {};
    CHECK(svg_rasterize(&(svg_rasterize_opts){.document = doc, .width = 0, .height = 8}, &pixmap) ==
          FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(pixmap.pixels == nullptr);
    CHECK(svg_rasterize(
              &(svg_rasterize_opts){
                  .document = doc, .width = 8, .height = 8, .format = (svg_pixel_format)99},
              &pixmap) == FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(pixmap.pixels == nullptr);
    /* An out-of-range supersample factor is refused by flux, and that refusal
     * must reach the caller rather than being silently clamped. */
    CHECK(svg_rasterize(&(svg_rasterize_opts){.document = doc,
                                              .width = 8,
                                              .height = 8,
                                              .format = SVG_PIXELS_RGBA8_PREMUL,
                                              .supersample = 999},
                        &pixmap) != FLUX_OK);
    svg_pixmap_deinit(&pixmap);
    svg_release(doc);
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
    TEST_REPORT();
}
