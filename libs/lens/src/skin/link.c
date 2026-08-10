/* skin/link.c — default link skin (ADR-0059): plain text at rest; on hover
 * the accent underline fades in (alpha follows the hover ease). ADR-0061:
 * default-skin transitions are colour/alpha only — geometry animation is
 * caller-skin territory. */

#include "../internal.h"

void lensi_skin_link(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    float h = rec->bounds.h;
    float tm_h = rec->content.text.height;

    float text_y = fmaxf((h - tm_h) * 0.5f - 1.0f, 0.0f);
    flux_color fg = disabled ? rs->disabled
                             : lensi_lerp_color(rs->fg, rs->accent, rec->hover_t * 0.35f);
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {0.0f, text_y, 0.0f, 0.0f},
                                        .color = fg,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size,
                                        .text_weight = 0.0f});

    if (rec->hover_t > 0.001f) {
        /* Neutral form (ADR-0061): full-width underline whose alpha eases in
         * with the hover float — hover feedback is colour/alpha only; width
         * growth was the last eased-geometry signature in the default skins. */
        float underline_y = fminf(text_y + tm_h + 2.0f, h - 1.5f);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0.0f, underline_y, rec->content.text.width, 1.5f},
                                            .color = lensi_color_alpha(rs->accent,
                                                                       (uint8_t)(255.0f * rec->hover_t)),
                                            .radius = 0.75f});
    }
}
