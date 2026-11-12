/* fuzz_reticle_parse.c — feed arbitrary bytes through the SVG compiler (ADR-0150).
 *
 * The ADR states that "untrusted input is the normal case": theme
 * directories, application bundles and downloads all reach `reticle_parse`.
 * This harness is that claim's executable half. It interprets the input
 * as SVG source, parses it, walks the resulting IR, and rasterizes it
 * through the headless software canvas, so the whole pipeline —
 * tag scanner, attribute parsing, path grammar, transform and gradient
 * resolution, flattening, coverage rasterization and pixmap ownership —
 * runs on every input.
 *
 * The compiler must:
 *
 *   - terminate on every input (no hang, no runaway nesting);
 *   - never read past the caller's (ptr, len) pair;
 *   - refuse a document that exceeds an explicit limit rather than
 *     truncate it (ADR-0150), so a successful parse always yields a
 *     document consistent with its own reported IR;
 *   - never retain a buffer on an error path (ASan reports the leak and
 *     `reticle_live_documents` must return to its entry value).
 *
 * Parse success/failure is otherwise irrelevant to fuzzing: every path
 * through the scanner and the ceilings is the target.
 *
 * Seeds in corpus/reticle_parse cover: a valid minimal document, the
 * gradient and transform surface, truncation at every structural
 * boundary, unterminated tags and strings, hostile numbers (NaN, inf,
 * 1e999999, overflow-length integers), unknown and malformed elements,
 * and nesting far beyond the `max_depth` ceiling. */

#include <reticle/reticle.h>

#include <math.h>
#include <stdint.h>

/* A document whose reported IR contradicts itself is a torn parse, not a
 * rejected one: the ceiling contract says a document is either refused
 * whole or compiled coherently. */
static bool ir_is_coherent(const reticle_document *doc) {
    flux_rect viewport = reticle_viewport(doc);
    if (!isfinite(viewport.w) || !isfinite(viewport.h) || viewport.w < 0.0f || viewport.h < 0.0f)
        return false;

    uint32_t shapes = reticle_shape_count(doc);
    for (uint32_t i = 0; i < shapes; ++i) {
        reticle_shape_view shape = {};
        if (reticle_shape_at(doc, i, &shape) != FLUX_OK)
            return false;

        /* Bounds exclude the stroke, so they may legitimately be empty
         * (a lone line) but never inverted or non-finite. */
        if (!isfinite(shape.bounds.x) || !isfinite(shape.bounds.y) || !isfinite(shape.bounds.w) ||
            !isfinite(shape.bounds.h) || shape.bounds.w < 0.0f || shape.bounds.h < 0.0f)
            return false;

        if (!(shape.opacity >= 0.0f && shape.opacity <= 1.0f) || !isfinite(shape.stroke_width) ||
            shape.stroke_width < 0.0f || !isfinite(shape.miter_limit) || shape.miter_limit < 0.0f)
            return false;

        for (uint32_t c = 0; c < shape.cmd_count; ++c) {
            if (shape.cmds[c].verb > RETICLE_PATH_CLOSE)
                return false;
            for (unsigned p = 0; p < 6; ++p) {
                if (!isfinite(shape.cmds[c].params[p]))
                    return false;
            }
        }

        /* A paint naming a gradient must resolve to one. */
        for (int half = 0; half < 2; ++half) {
            const reticle_paint *paint = half ? &shape.stroke : &shape.fill;
            if ((paint->kind == RETICLE_PAINT_LINEAR_GRADIENT ||
                 paint->kind == RETICLE_PAINT_RADIAL_GRADIENT)) {
                reticle_gradient_view gradient = {};
                if (reticle_gradient_at(doc, paint->gradient, &gradient) != FLUX_OK)
                    return false;
                if (gradient.stop_count > FLUX_GRADIENT_MAX_STOPS)
                    return false;
            }
            if (!isfinite(paint->opacity) || paint->opacity < 0.0f || paint->opacity > 1.0f)
                return false;
            if (paint->kind > RETICLE_PAINT_RADIAL_GRADIENT)
                return false;
        }
    }

    /* Attribute lookup is a borrowed view; a name the document never
     * carried must read as absent rather than as a stale pointer. */
    static const char *const PROBES[] = {"viewBox", "width", "height", "id", "fill"};
    for (unsigned i = 0; i < sizeof PROBES / sizeof PROBES[0]; ++i) {
        const char *value = reticle_attribute(doc, PROBES[i]);
        if (value && value[0] == '\0' && reticle_attribute(doc, PROBES[i]) != value)
            return false;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The leak lane's contract: parse and release must balance exactly.
     * Compared against the entry value rather than against zero so the
     * harness composes with anything else that holds a document. */
    uint32_t live_before = reticle_live_documents();

    reticle_document *doc = nullptr;
    flux_result r = reticle_parse(
        &(reticle_parse_opts){
            .text = (const char *)data,
            .length = size,
        },
        &doc);

    int verdict = 0;
    if (r == FLUX_OK && doc) {
        if (!ir_is_coherent(doc)) {
            verdict = 1;
        } else {
            /* Rasterize through the deviceless software canvas so the
             * flattening and coverage stages see the hostile geometry.
             * A 1x1 bake keeps the campaign fast; the geometry, not the
             * output extent, is what carries the risk. */
            reticle_pixmap pixmap = {};
            flux_result rr = reticle_rasterize(
                &(reticle_rasterize_opts){
                    .document = doc,
                    .width = 8,
                    .height = 8,
                    .supersample = 1,
                    .format = RETICLE_PIXELS_RGBA8_PREMUL,
                    .paint = RETICLE_PAINT_THEME,
                },
                &pixmap);
            if (rr == FLUX_OK) {
                if (!pixmap.pixels || pixmap.width != 8 || pixmap.height != 8 ||
                    pixmap.stride < pixmap.width * 4)
                    verdict = 2;
            }
            reticle_pixmap_deinit(&pixmap);
            if (pixmap.pixels)
                verdict = 3; /* deinit must zero the struct */
        }
    } else if (doc) {
        /* A refused parse must not hand back a document. */
        verdict = 4;
    }
    if (doc)
        reticle_release(doc);

    if (reticle_live_documents() != live_before)
        verdict = 5;
    return verdict;
}
