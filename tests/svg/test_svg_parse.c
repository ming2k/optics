/* test_svg_parse.c — the document contract: viewport, shape enumeration,
 * attribute passthrough, refcounting, and the "no partial document" rule.
 *
 * Every check here is about what the parser promises a consumer, because every
 * consumer (cursor themes, icon sets, tray bitmaps) builds on exactly these four
 * facts: how big the art is, which shapes exist, what metadata came back, and
 * that a malformed document yields nothing rather than something half-built. */

#include "test_helpers.h"

#include <stdlib.h>

#include <svg/svg.h>

static svg_document *parse(const char *text) {
    svg_document *doc = nullptr;
    if (svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = text}, &doc) != FLUX_OK)
        return nullptr;
    return doc;
}

static void viewport_from_viewbox(void) {
    svg_document *doc = parse("<svg viewBox=\"0 0 256 256\"><rect x=\"0\" y=\"0\" width=\"4\" "
                              "height=\"4\"/></svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    flux_rect viewport = svg_viewport(doc);
    CHECK_NEAR(viewport.x, 0.0f, 1e-6);
    CHECK_NEAR(viewport.y, 0.0f, 1e-6);
    CHECK_NEAR(viewport.w, 256.0f, 1e-6);
    CHECK_NEAR(viewport.h, 256.0f, 1e-6);
    CHECK(svg_shape_count(doc) == 1);
    svg_release(doc);
}

static void viewport_from_width_height(void) {
    svg_document *doc = parse("<svg width=\"48\" height=\"24\"><rect x=\"0\" y=\"0\" width=\"4\" "
                              "height=\"4\"/></svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    flux_rect viewport = svg_viewport(doc);
    CHECK_NEAR(viewport.w, 48.0f, 1e-6);
    CHECK_NEAR(viewport.h, 24.0f, 1e-6);
    svg_release(doc);
}

static void viewport_falls_back_to_geometry(void) {
    /* Neither viewBox nor width/height: the union of the shapes, so a document
     * that only says "draw this path" still has a usable aspect ratio. */
    svg_document *doc = parse("<svg><rect x=\"10\" y=\"20\" width=\"30\" height=\"5\"/></svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    flux_rect viewport = svg_viewport(doc);
    CHECK_NEAR(viewport.x, 10.0f, 1e-4);
    CHECK_NEAR(viewport.y, 20.0f, 1e-4);
    CHECK_NEAR(viewport.w, 30.0f, 1e-4);
    CHECK_NEAR(viewport.h, 5.0f, 1e-4);
    svg_release(doc);
}

static void empty_document_reports_a_usable_box(void) {
    /* A zero-extent viewport would make every caller's aspect-ratio division a
     * footgun; 1x1 is the documented answer. */
    svg_document *doc = parse("<svg viewBox=\"0 0 0 0\"></svg>");
    if (!doc)
        return; /* refusing the document outright is also acceptable */
    flux_rect viewport = svg_viewport(doc);
    CHECK(viewport.w > 0.0f && viewport.h > 0.0f);
    CHECK(svg_shape_count(doc) == 0);
    svg_release(doc);
}

static void metadata_attributes_come_back(void) {
    /* `data-*` is the only channel SVG has for application metadata: the cursor
     * themes stamp their hotspot there. It must survive parsing verbatim. */
    svg_document *doc = parse("<svg viewBox=\"0 0 256 256\" data-hotspot-x=\"64\" "
                              "data-hotspot-y=\"28.5\">"
                              "<path d=\"M0 0L10 0L10 10Z\"/></svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    CHECK_STR(svg_attribute(doc, "data-hotspot-x"), "64");
    CHECK_STR(svg_attribute(doc, "data-hotspot-y"), "28.5");
    CHECK(svg_attribute(doc, "data-missing") == nullptr);
    /* viewBox is geometry, not metadata; it is read through svg_viewport. */
    CHECK(svg_attribute(doc, "viewBox") == nullptr);
    svg_release(doc);
}

static void malformed_documents_yield_nothing(void) {
    const char *bad[] = {
        "<svg viewBox=\"0 0 8 8\"><rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>", /* unclosed */
        "<svg viewBox=\"0 0 8 8\"><path d=\"M0 0 L\"/></svg>",                       /* bad path */
        "",                                                                          /* empty */
        "not svg at all",                                                            /* no markup */
        ">>>>",                                                                      /* no tag */
        "<svg viewBox=\"0 0 8 8\"><path d=\"M 1 2 Q 3\"/></svg>", /* short quad */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        svg_document *doc = nullptr;
        flux_result result =
            svg_parse(&(svg_parse_opts){.length = SVG_LENGTH_MEASURE, .text = bad[i]}, &doc);
        CHECK(result != FLUX_OK);
        CHECK(doc == nullptr);
        if (doc)
            svg_release(doc);
    }
}

static void retention_outlives_the_caller(void) {
    svg_document *doc = parse("<svg viewBox=\"0 0 8 8\"><rect width=\"4\" height=\"4\"/></svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    svg_document *second = svg_retain(doc);
    CHECK(second == doc);
    svg_document *third = svg_retain(second);
    CHECK(third == doc);
    /* Releasing three of four references must not free; the last one does. */
    svg_release(third);
    svg_release(second);
    CHECK(svg_shape_count(doc) == 1);
    svg_release(doc);
    /* svg_release(NULL) is a documented no-op. */
    svg_release(nullptr);
}

static void explicit_length_is_honoured(void) {
    /* A non-NUL-terminated span must parse: callers hold slices of a larger
     * buffer (an archive entry, an IPC payload). */
    const char *source = "<svg viewBox=\"0 0 8 8\"><rect width=\"4\" height=\"4\"/></svg>TAIL";
    svg_document *doc = nullptr;
    flux_result result =
        svg_parse(&(svg_parse_opts){.text = source, .length = strlen(source) - 4}, &doc);
    CHECK(result == FLUX_OK);
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    CHECK(svg_shape_count(doc) == 1);
    svg_release(doc);
}

static void comments_and_processing_instructions_are_skipped(void) {
    svg_document *doc = parse("<?xml version=\"1.0\"?>"
                              "<!-- an editor's header -->"
                              "<svg viewBox=\"0 0 8 8\">"
                              "<!-- inside -->"
                              "<rect width=\"4\" height=\"4\"/>"
                              "</svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    CHECK(svg_shape_count(doc) == 1);
    svg_release(doc);
}

static void unknown_elements_do_not_break_a_document(void) {
    /* Forward compatibility: an editor's private element must not cost the whole
     * document. */
    svg_document *doc = parse("<svg viewBox=\"0 0 8 8\">"
                              "<sodipodi:namedview pagecolor=\"#fff\"/>"
                              "<rect width=\"4\" height=\"4\"/>"
                              "<fancy-editor-thing data-x=\"1\"><nested/></fancy-editor-thing>"
                              "</svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    CHECK(svg_shape_count(doc) == 1);
    svg_release(doc);
}

static void opaque_container_groups_do_not_unbalance_the_style_stack(void) {
    /* `<defs><g></g></defs>` is how an icon set groups a symbol's paths. Nothing
     * inside an opaque container pushes a style frame (the geometry is inert),
     * so nothing may pop one either: doing so discards the document's inherited
     * fill and recolours every shape after the defs block. The art still renders,
     * so only the colour check catches it. */
    svg_document *doc = parse("<svg fill=\"#ff0000\" viewBox=\"0 0 10 10\">"
                              "<defs><g></g></defs>"
                              "<rect width=\"5\" height=\"5\"/>"
                              "</svg>");
    CHECK(doc != nullptr);
    if (!doc) {
        return;
    }
    CHECK(svg_shape_count(doc) == 1);
    svg_shape_view shape = {};
    CHECK(svg_shape_at(doc, 0, &shape) == FLUX_OK);
    CHECK(shape.fill.kind == SVG_PAINT_SOLID);
    CHECK((shape.fill.color & 0x00FFFFFFu) == 0x00FF0000u); /* still the root's red */
    svg_release(doc);

    /* The same for the other opaque containers that a real icon set nests. */
    static const char *const containers[] = {
        "<symbol id=\"s\"><g/></symbol>",
        "<clipPath id=\"c\"><g/></clipPath>",
        "<mask id=\"m\"><g></g></mask>",
        "<pattern id=\"p\"><g></g></pattern>",
        "<marker id=\"k\"><g></g></marker>",
        /* A nested `<svg>` inside an opaque container *does* push a frame (the
         * root-seed rule is unconditional), so its close must pop one: a guard
         * that simply skipped every close inside a container would reject this
         * perfectly well-formed document. */
        "<defs><svg id=\"n\"></svg></defs>",
        "<defs><svg id=\"n\"/></defs>",
    };
    for (size_t i = 0; i < sizeof containers / sizeof containers[0]; ++i) {
        char src[256];
        snprintf(src, sizeof src,
                 "<svg fill=\"#ff0000\" viewBox=\"0 0 10 10\">%s"
                 "<rect width=\"5\" height=\"5\"/></svg>",
                 containers[i]);
        svg_document *inner = parse(src);
        CHECK(inner != nullptr);
        if (!inner) {
            continue;
        }
        CHECK(svg_shape_count(inner) == 1);
        svg_shape_view view = {};
        CHECK(svg_shape_at(inner, 0, &view) == FLUX_OK);
        CHECK((view.fill.color & 0x00FFFFFFu) == 0x00FF0000u);
        svg_release(inner);
    }
}

static void unmatched_close_tags_are_not_a_document(void) {
    /* An extra `</g>` used to pop the *root seed* frame silently, so the parse
     * reported success with the document's inherited style destroyed. Broken
     * nesting cannot be rendered faithfully, and "never a partial document" is
     * the contract — so this must fail, not degrade. */
    CHECK(parse("<svg viewBox=\"0 0 10 10\"></g><rect width=\"5\" height=\"5\"/></svg>") ==
          nullptr);
    /* The same underflow reached through a self-closing group. */
    CHECK(parse("<svg viewBox=\"0 0 10 10\"><g/></g><rect width=\"5\" height=\"5\"/></svg>") ==
          nullptr);
    /* A well-formed document is still accepted. */
    svg_document *ok = parse("<svg viewBox=\"0 0 10 10\"><g></g><rect width=\"5\" height=\"5\"/>"
                             "</svg>");
    CHECK(ok != nullptr);
    svg_release(ok);
}

/* A container marker whose close sequence lies outside the caller's span must be
 * refused, not scanned for. `svg_parse` takes a `(ptr, len)` pair, so the buffer
 * carries no terminator to find: an unbounded strstr() walks past the end reading
 * whatever heap bytes follow (ASan: heap-buffer-overflow, found by the svg_parse
 * fuzz harness in 828 executions).
 *
 * The span is copied into an exactly-sized allocation rather than pointed at a
 * string literal. A literal is NUL-terminated in memory even when `length`
 * excludes the terminator, so strstr() would stop on that NUL and the overread
 * would stay invisible here while remaining reachable from any real (ptr, len)
 * caller. Exact sizing is what makes this test able to fail. */
static char *span_dup(const char *src, size_t len) {
    char *buf = (char *)malloc(len); /* deliberately no room for a terminator */
    if (!buf)
        return nullptr;
    memcpy(buf, src, len);
    return buf;
}

static bool parse_refuses(const char *src, size_t len) {
    char *buf = span_dup(src, len);
    CHECK(buf != nullptr);
    if (!buf)
        return false;
    svg_document *doc = nullptr;
    flux_result r = svg_parse(&(svg_parse_opts){.text = buf, .length = len}, &doc);
    free(buf);
    CHECK(doc == nullptr);
    return r == FLUX_ERROR_INVALID_ARGUMENT;
}

static void unterminated_containers_are_refused(void) {
    static const char CDATA[] = "<![CDATA[<svg viewBox=\"0 0 8 8\">";
    CHECK(parse_refuses(CDATA, sizeof CDATA - 1));

    static const char COMMENT[] = "<svg viewBox=\"0 0 8 8\"><!-- unterminated";
    CHECK(parse_refuses(COMMENT, sizeof COMMENT - 1));

    static const char PI[] = "<svg viewBox=\"0 0 8 8\"><?xml unterminated";
    CHECK(parse_refuses(PI, sizeof PI - 1));

    /* A well-formed container inside an exactly-sized, unterminated span must
     * still be accepted: bounding the search must not turn a valid document
     * into a refused one. */
    static const char TIGHT[] =
        "<svg viewBox=\"0 0 8 8\"><!--x--><![CDATA[y]]><rect width=\"8\" height=\"8\"/></svg>";
    char *buf = span_dup(TIGHT, sizeof TIGHT - 1);
    CHECK(buf != nullptr);
    if (buf) {
        svg_document *doc = nullptr;
        CHECK(svg_parse(&(svg_parse_opts){.text = buf, .length = sizeof TIGHT - 1}, &doc) ==
              FLUX_OK);
        free(buf);
        CHECK(doc != nullptr);
        if (doc)
            svg_release(doc);
    }
}

int main(void) {
    viewport_from_viewbox();
    viewport_from_width_height();
    viewport_falls_back_to_geometry();
    empty_document_reports_a_usable_box();
    metadata_attributes_come_back();
    malformed_documents_yield_nothing();
    retention_outlives_the_caller();
    explicit_length_is_honoured();
    comments_and_processing_instructions_are_skipped();
    unknown_elements_do_not_break_a_document();
    opaque_container_groups_do_not_unbalance_the_style_stack();
    unmatched_close_tags_are_not_a_document();
    unterminated_containers_are_refused();
    TEST_REPORT();
}
