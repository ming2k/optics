/* fuzz_text_shape.c — feed arbitrary (and arbitrarily truncated) UTF-8
 * through the measure-only text seam.
 *
 * glyph_create accepts a nullptr device ("measure-only"), so the whole
 * measure / caret / selection / visual-order surface is reachable without
 * a Vulkan ICD — no GPU coupling in CI. The input bytes are used three
 * ways:
 *
 *   - as UTF-8 text, truncated at every possible boundary via `len`:
 *     multibyte sequences cut mid-codepoint, lone continuation bytes,
 *     overlong encodings, embedded NULs;
 *   - as a byte index (cursor) for the byte<->x mapping and visual move;
 *   - as a selection range for glyph_selection_rects.
 *
 * The invariants asserted (beyond "no crash / no leak" under ASan):
 *   - measure results are finite and non-negative;
 *   - every returned selection rect has x0 <= x1;
 *   - visual_move lands within [0, len];
 *   - byte_for_x stays within [0, len], so caret motion cannot run away.
 *
 * Real bugs surface as UB / leaks (ASan), runaway loops, or a violated
 * invariant below. The committed seed corpus
 * (seeds under corpus/text_shape/) covers valid multibyte, bidi mixes, lone
 * continuation bytes, truncated sequences, overlong encodings,
 * embedded NULs, leading combining marks, and long mixed strings. */

#include <math.h>
#include <optics/gfx/core.h>
#include <optics/text.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static optics_text_metrics measure(optics_text_ctx *t, const char *s, size_t len,
                                   const optics_text_style *style) {
    return optics_text_measure(t,
                               &(optics_text_measure_opts){.utf8 = s, .len = len, .style = style});
}

static float caret_x(optics_text_ctx *t, const char *s, size_t len, size_t byte,
                     const optics_text_style *style) {
    return optics_text_x_for_byte(
        t, &(optics_text_x_for_byte_opts){.utf8 = s, .len = len, .byte = byte, .style = style});
}

static size_t caret_byte(optics_text_ctx *t, const char *s, size_t len, float x,
                         const optics_text_style *style) {
    return optics_text_byte_for_x(
        t, &(optics_text_byte_for_x_opts){.utf8 = s, .len = len, .local_x = x, .style = style});
}

static size_t visual_move(optics_text_ctx *t, const char *s, size_t len, size_t byte, bool forward,
                          const optics_text_style *style) {
    return optics_text_visual_move(
        t, &(optics_text_visual_move_opts){
               .utf8 = s, .len = len, .byte = byte, .forward = forward, .style = style});
}

static int selection(optics_text_ctx *t, const char *s, size_t len, size_t lo, size_t hi,
                     const optics_text_style *style, optics_text_xrange *out, int max) {
    return optics_text_selection_rects(t, &(optics_text_selection_rects_opts){
                                              .utf8 = s,
                                              .len = len,
                                              .lo = lo,
                                              .hi = hi,
                                              .style = style,
                                              .out = out,
                                              .max = max,
                                          });
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0)
        return 0;

    optics_text_ctx *t = nullptr;
    if (optics_text_create(&(optics_text_desc){}, &t) != OPTICS_GFX_OK)
        return 0;

    /* Text = the whole input, len sweeps 0..size so every truncation
     * boundary of every multibyte sequence is exercised. */
    const char *text = (const char *)data;
    float size_px = 8.0f + (float)(data[0] % 24); /* 8..31 px, deterministic */

    for (size_t len = 0; len <= size; len++) {
        optics_text_style st = {};
        st.size_px = size_px;

        optics_text_metrics m = measure(t, text, len, &st);
        if (!isfinite(m.width) || !isfinite(m.height) || !isfinite(m.baseline) || m.width < 0.0f ||
            m.height < 0.0f) {
            fprintf(stderr, "fuzz_text_shape: non-finite/negative metrics at len=%zu\n", len);
            optics_text_release(t);
            return 1;
        }

        /* Caret mapping: a byte index derived from the input itself. */
        size_t byte = (len > 0) ? (size_t)data[(len - 1) % size] % (len + 1) : 0;
        float x = caret_x(t, text, len, byte, &st);
        if (!isfinite(x) || x < 0.0f) {
            fprintf(stderr, "fuzz_text_shape: bad x_for_byte at len=%zu byte=%zu\n", len, byte);
            optics_text_release(t);
            return 1;
        }
        size_t back = caret_byte(t, text, len, x, &st);
        if (back > len) {
            fprintf(stderr, "fuzz_text_shape: byte_for_x out of range at len=%zu\n", len);
            optics_text_release(t);
            return 1;
        }

        /* Visual movement (LTR/RTL bidi-aware cursor steps). */
        bool forward = (data[0] & 1u) != 0;
        size_t moved = visual_move(t, text, len, byte, forward, &st);
        if (moved > len) {
            fprintf(stderr, "fuzz_text_shape: visual_move out of range at len=%zu\n", len);
            optics_text_release(t);
            return 1;
        }

        /* Selection: [lo, hi) inside [0, len]. One rect per visual line;
         * this text is a single paragraph, so 0..4 rects are legal. */
        size_t lo = (len > 0) ? (size_t)data[0] % (len + 1) : 0;
        size_t hi = (len > 0) ? (size_t)data[(len - 1) % size] % (len + 1) : 0;
        if (lo > hi) {
            size_t tmp = lo;
            lo = hi;
            hi = tmp;
        }
        optics_text_xrange rects[4];
        int n = selection(t, text, len, lo, hi, &st, rects, 4);
        if (n > 4) {
            fprintf(stderr, "fuzz_text_shape: selection_rects overflow at len=%zu\n", len);
            optics_text_release(t);
            return 1;
        }
        for (int r = 0; r < n; r++) {
            if (!isfinite(rects[r].x0) || !isfinite(rects[r].x1) || rects[r].x0 > rects[r].x1) {
                fprintf(stderr, "fuzz_text_shape: inverted selection rect at len=%zu\n", len);
                optics_text_release(t);
                return 1;
            }
        }
    }

    optics_text_release(t);
    return 0;
}
