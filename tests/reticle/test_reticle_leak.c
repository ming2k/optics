/* test_svg_leak.c — the drain assertion for the `svg` resource kind
 * (INV-ARCH-LEDGER, ADR-0130).
 *
 * A parsed document is device-less by design (ADR-0136), so no flux device
 * ledger can own it; `reticle_live_documents` is the drain counter instead. The
 * assertions here are exact deltas, never a tolerance: the regression this lane
 * exists to catch is an off-by-one in the refcount, and `<= +1` would swallow it.
 *
 * Every path that can create a document is exercised, including the ones that
 * fail partway through parsing, because a mid-parse failure is exactly where a
 * leaked document would hide. */

#include "test_helpers.h"

#include <reticle/reticle.h>
#include <stdlib.h>
#include <string.h>

static uint32_t live(void) {
    return reticle_live_documents();
}

// Multi-line documents are named rather than concatenated inside the array.
// `-Wstring-concatenation` treats adjacent literals in an array initializer as
// a likely missing comma, and the warning is right to: naming the document
// keeps the array a list of identifiers, where a missing comma cannot compile
// silently.
static const char *const DOC_GRADIENT =
    "<svg viewBox=\"0 0 100 100\"><defs>"
    "<linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#FFF\"/>"
    "<stop offset=\"1\" stop-color=\"#000\"/></linearGradient></defs>"
    "<rect width=\"100\" height=\"100\" fill=\"url(#g)\"/></svg>";

static const char *const DOC_HOTSPOT =
    "<svg viewBox=\"0 0 8 8\" data-hotspot-x=\"4\" data-hotspot-y=\"4\">"
    "<circle cx=\"4\" cy=\"4\" r=\"3\"/></svg>";

static const char *const DOCUMENTS[] = {
    "<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#FF0000\"/></svg>",
    "<svg viewBox=\"0 0 24 24\"><path d=\"M2 2 L22 22\" stroke=\"#000\" fill=\"none\"/></svg>",
    DOC_GRADIENT,
    DOC_HOTSPOT,
};

static void every_document_drains(void) {
    const uint32_t baseline = live();
    for (size_t i = 0; i < sizeof DOCUMENTS / sizeof DOCUMENTS[0]; ++i) {
        reticle_document *doc = nullptr;
        CHECK(reticle_parse(
                  &(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = DOCUMENTS[i]},
                  &doc) == FLUX_OK);
        CHECK(doc != nullptr);
        if (!doc)
            continue;
        CHECK(live() == baseline + 1u);
        reticle_release(doc);
        CHECK(live() == baseline);
    }
}

static void failed_parses_do_not_leak(void) {
    const uint32_t baseline = live();
    /* Each of these allocates part of a parser before failing: a malformed
     * geometry, an exceeded ceiling, an unterminated document. */
    const char *const BAD[] = {
        "<svg viewBox=\"0 0 8 8\"><path d=\"M0 0 L\"/></svg>",
        "<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\"/>",
        "",
        "no markup here at all",
        "<svg viewBox=\"0 0 8 8\"><g><g><rect width=\"1\" height=\"1\"/></svg>",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; ++i) {
        reticle_document *doc = nullptr;
        CHECK(reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = BAD[i]},
                            &doc) != FLUX_OK);
        CHECK(doc == nullptr);
        if (doc)
            reticle_release(doc);
        CHECK(live() == baseline);
    }

    /* A ceiling failure happens after shapes have been allocated. */
    reticle_parse_limits limits = {.max_shapes = 1};
    reticle_document *doc = nullptr;
    CHECK(reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE,
                                              .text = "<svg viewBox=\"0 0 8 8\">"
                                                      "<rect width=\"1\" height=\"1\"/>"
                                                      "<rect width=\"1\" height=\"1\"/></svg>",
                                              .limits = &limits},
                        &doc) != FLUX_OK);
    CHECK(doc == nullptr);
    CHECK(live() == baseline);
}

static void retention_balances(void) {
    const uint32_t baseline = live();
    reticle_document *doc = nullptr;
    if (reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = DOCUMENTS[0]},
                      &doc) != FLUX_OK ||
        !doc)
        return;
    /* Retention is not a new document; it is a new reference to one. Three
     * references, three releases, one document. */
    CHECK(reticle_retain(doc) == doc);
    CHECK(reticle_retain(doc) == doc);
    CHECK(live() == baseline + 1u);
    reticle_release(doc);
    reticle_release(doc);
    CHECK(live() == baseline + 1u);
    reticle_release(doc);
    CHECK(live() == baseline);
}

static void rasterization_is_document_neutral(void) {
    /* Rasterizing must not retain or clone the document: a cursor cache calls it
     * once per (shape, scale) and a leaked reference per call would look exactly
     * like a slowly growing RAM footprint nobody can attribute. */
    const uint32_t baseline = live();
    reticle_document *doc = nullptr;
    if (reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = DOCUMENTS[3]},
                      &doc) != FLUX_OK ||
        !doc)
        return;
    const uint32_t after_parse = live();
    for (uint32_t size = 8; size <= 32; size += 8) {
        reticle_pixmap pixmap = {};
        CHECK(reticle_rasterize(
                  &(reticle_rasterize_opts){
                      .document = doc,
                      .width = size,
                      .height = size,
                      .format = RETICLE_PIXELS_RGBA8_PREMUL,
                      .paint = RETICLE_PAINT_MONOCHROME,
                      .color = flux_color_rgba(255, 255, 255, 255),
                  },
                  &pixmap) == FLUX_OK);
        reticle_pixmap_deinit(&pixmap);
        CHECK(live() == after_parse);
    }
    reticle_release(doc);
    CHECK(live() == baseline);
}

static void repeated_parse_release_does_not_drift(void) {
    /* A hundred cycles, because a single one cannot distinguish a balanced
     * refcount from one that leaks a byte per parse. */
    const uint32_t baseline = live();
    for (uint32_t i = 0; i < 100; ++i) {
        reticle_document *doc = nullptr;
        if (reticle_parse(
                &(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = DOCUMENTS[2]},
                &doc) != FLUX_OK)
            break;
        CHECK(live() == baseline + 1u);
        reticle_release(doc);
    }
    CHECK(live() == baseline);
}

int main(void) {
    /* The counter must start where this binary found it; the lane asserts the
     * delta, so a document left alive by another test in this process would be a
     * real failure, not noise. */
    const uint32_t baseline = live();
    every_document_drains();
    failed_parses_do_not_leak();
    retention_balances();
    rasterization_is_document_neutral();
    repeated_parse_release_does_not_drift();
    CHECK(live() == baseline);
    TEST_REPORT();
}
