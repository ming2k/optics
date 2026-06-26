/* mono_stub.c — measure-only monospace fallback.
 *
 * Geometry-correct fixed-advance metrics, always compiled. Used when the
 * real backend is absent (no FreeType / HarfBuzz / Fontconfig) or when a
 * context degrades, so measuring still produces sensible rectangles; glyph
 * drawing is the real backend's job. */

#include <flux-text/text.h>

#define TXT_MONO_ADVANCE 0.6f  /* advance / size_px */
#define TXT_MONO_LINE 1.4f     /* line height / size_px */
#define TXT_MONO_BASELINE 1.1f /* baseline / size_px */

/* Count Unicode scalar values in a UTF-8 run of `len` bytes (leading
 * bytes only), so wide characters advance once, not per byte. */
static size_t utf8_count(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            n++;
    return n;
}

flux_text_metrics txt_text_measure_mono(const char *utf8, size_t len, float size_px, float weight) {
    (void)weight;
    size_t glyphs = utf8 ? utf8_count(utf8, len) : 0;
    flux_text_metrics m = {
        .width = (float)glyphs * size_px * TXT_MONO_ADVANCE,
        .height = size_px * TXT_MONO_LINE,
        .baseline = size_px * TXT_MONO_BASELINE,
    };
    return m;
}
