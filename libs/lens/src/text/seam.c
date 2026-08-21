/* seam.c — lens's thin seam over the shared flux-text engine.
 *
 * These helpers take an lens (routing to ui->text), apply lens's label
 * conventions (the visible prefix before a "##" id suffix), and convert
 * between lens and flux-text metric types. All shaping, layout, caret
 * mapping, and the monospace fallback live in flux-text; ui->text is always
 * usable (it degrades internally), so there is no backend branching here. */

#include "../internal.h"

static flux_text_style style_of(const lens *ui, float size_px, float weight) {
    return (flux_text_style){.size_px = size_px,
                             .weight = weight,
                             .family = (flux_text_family)(ui ? ui->text_family : 0)};
}

static lens_text_metrics to_fx(flux_text_metrics m) {
    return (lens_text_metrics){m.width, m.height, m.baseline};
}

lens_text_metrics lens_text_measure_ex(lens *ui, lens_font *font, const char *utf8, float size_px,
                                       float weight) {
    (void)font;
    if (!utf8 || !utf8[0])
        return (lens_text_metrics){0};
    flux_text_style s = style_of(ui, size_px, weight);
    return to_fx(flux_text_measure(ui ? ui->text : NULL, utf8, strlen(utf8), &s));
}

lens_text_metrics lens_text_measure(lens *ui, lens_font *font, const char *utf8, float size_px) {
    return lens_text_measure_ex(ui, font, utf8, size_px, 0.0f);
}

/* Measure the visible prefix of a label (everything before "##"). flux-text
 * takes (ptr, len), so the prefix is passed directly without copying. */
lens_text_metrics lensi_text_measure_label(lens *ui, const char *label, float size_px,
                                           float weight) {
    if (!label)
        return (lens_text_metrics){0};
    size_t vlen = lensi_label_visible_len(label);
    if (vlen == 0)
        return (lens_text_metrics){0};
    flux_text_style s = style_of(ui, size_px, weight);
    return to_fx(flux_text_measure(ui ? ui->text : NULL, label, vlen, &s));
}

float lensi_text_caret_x(lens *ui, const char *utf8, size_t byte, float size_px, float weight) {
    if (!utf8 || byte == 0)
        return 0.0f;
    flux_text_style s = style_of(ui, size_px, weight);
    return flux_text_x_for_byte(ui ? ui->text : NULL, utf8, strlen(utf8), byte, &s);
}

size_t lensi_text_caret_byte(lens *ui, const char *utf8, float local_x, float size_px,
                             float weight) {
    if (!utf8 || !utf8[0])
        return 0;
    flux_text_style s = style_of(ui, size_px, weight);
    return flux_text_byte_for_x(ui ? ui->text : NULL, utf8, strlen(utf8), local_x, &s);
}

int lensi_text_sel_rects(lens *ui, const char *utf8, size_t lo, size_t hi, float size_px,
                         float weight, lens_text_xrange *out, int max) {
    if (!utf8 || lo >= hi || max <= 0)
        return 0;
    flux_text_style s = style_of(ui, size_px, weight);
    return flux_text_selection_rects(ui ? ui->text : NULL, utf8, strlen(utf8), lo, hi, &s, out,
                                     max);
}

size_t lensi_text_caret_visual(lens *ui, const char *utf8, size_t byte, bool forward, float size_px,
                               float weight) {
    if (!utf8)
        return byte;
    flux_text_style s = style_of(ui, size_px, weight);
    return flux_text_visual_move(ui ? ui->text : NULL, utf8, strlen(utf8), byte, forward, &s);
}

void lens_text_compact(lens *ui) {
    /* Thin forward: the policy (when to call) belongs to the host; the
     * mechanism lives in the engine. See ADR-0072 item 5. */
    if (ui)
        flux_text_compact(ui->text);
}
