/* fuzz_text_shape.c — feed arbitrary (and arbitrarily truncated) UTF-8
 * through the measure-only text seam.
 *
 * flux_text_create accepts a NULL device ("measure-only"), so the whole
 * measure / caret / selection / visual-order surface is reachable without
 * a Vulkan ICD — no GPU coupling in CI. The input bytes are used three
 * ways:
 *
 *   - as UTF-8 text, truncated at every possible boundary via `len`:
 *     multibyte sequences cut mid-codepoint, lone continuation bytes,
 *     overlong encodings, embedded NULs;
 *   - as a byte index (cursor) for the byte<->x mapping and visual move;
 *   - as a selection range for flux_text_selection_rects.
 *
 * The invariants asserted (beyond "no crash / no leak" under ASan):
 *   - measure results are finite and non-negative;
 *   - every returned selection rect has x0 <= x1;
 *   - visual_move lands within [0, len];
 *   - byte_for_x stays within [0, len], so caret motion cannot run away.
 *
 * Real bugs surface as UB / leaks (ASan), runaway loops, or a violated
 * invariant below. The committed seed corpus
 * (corpus/text_shape/*.bin) covers valid multibyte, bidi mixes, lone
 * continuation bytes, truncated sequences, overlong encodings,
 * embedded NULs, leading combining marks, and long mixed strings. */

#include <flux-text/text.h>
#include <flux/core.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0)
        return 0;

    flux_text *t = NULL;
    if (flux_text_create(&(flux_text_desc){0}, &t) != FLUX_OK)
        return 0;

    /* Text = the whole input, len sweeps 0..size so every truncation
     * boundary of every multibyte sequence is exercised. */
    const char *text = (const char *)data;
    float size_px = 8.0f + (float)(data[0] % 24); /* 8..31 px, deterministic */

    for (size_t len = 0; len <= size; len++) {
        flux_text_style st = {0};
        st.size_px = size_px;

        flux_text_metrics m = flux_text_measure(t, text, len, &st);
        if (!isfinite(m.width) || !isfinite(m.height) || !isfinite(m.baseline) || m.width < 0.0f ||
            m.height < 0.0f) {
            fprintf(stderr, "fuzz_text_shape: non-finite/negative metrics at len=%zu\n", len);
            flux_text_destroy(t);
            return 1;
        }

        /* Caret mapping: a byte index derived from the input itself. */
        size_t byte = (len > 0) ? (size_t)data[(len - 1) % size] % (len + 1) : 0;
        float x = flux_text_x_for_byte(t, text, len, byte, &st);
        if (!isfinite(x) || x < 0.0f) {
            fprintf(stderr, "fuzz_text_shape: bad x_for_byte at len=%zu byte=%zu\n", len, byte);
            flux_text_destroy(t);
            return 1;
        }
        size_t back = flux_text_byte_for_x(t, text, len, x, &st);
        if (back > len) {
            fprintf(stderr, "fuzz_text_shape: byte_for_x out of range at len=%zu\n", len);
            flux_text_destroy(t);
            return 1;
        }

        /* Visual movement (LTR/RTL bidi-aware cursor steps). */
        bool forward = (data[0] & 1u) != 0;
        size_t moved = flux_text_visual_move(t, text, len, byte, forward, &st);
        if (moved > len) {
            fprintf(stderr, "fuzz_text_shape: visual_move out of range at len=%zu\n", len);
            flux_text_destroy(t);
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
        flux_text_xrange rects[4];
        int n = flux_text_selection_rects(t, text, len, lo, hi, &st, rects, 4);
        if (n > 4) {
            fprintf(stderr, "fuzz_text_shape: selection_rects overflow at len=%zu\n", len);
            flux_text_destroy(t);
            return 1;
        }
        for (int r = 0; r < n; r++) {
            if (!isfinite(rects[r].x0) || !isfinite(rects[r].x1) || rects[r].x0 > rects[r].x1) {
                fprintf(stderr, "fuzz_text_shape: inverted selection rect at len=%zu\n", len);
                flux_text_destroy(t);
                return 1;
            }
        }
    }

    flux_text_destroy(t);
    return 0;
}
