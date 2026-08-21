/* skin/icon_button.c — default icon-button skin (ADR-0059). The pre-skin
 * emit section of the lens_icon_button* family, moved verbatim, then
 * neutralized per ADR-0061 item 7: the active state is a steady neutral
 * tint (resolved bg_pressed; theme: color_active) with a plain foreground
 * glyph — no accent rail, no accent active glyph. A rail/accent treatment
 * is caller-owned flavor: reachable through the style atoms on the record
 * or a custom skin, never a separate API. */

#include "../internal.h"

void lensi_skin_icon_button(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    bool rounded = rec->content.rounded;
    bool active_surface = rec->content.active_surface;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    /* The glyph only steps aside for a few pixels of breathing room —
     * subtracting the full theme padding would crush it on compact
     * fixed-size targets (a 32 px button would render an 8 px glyph). */
    float max_s = w < h ? w : h;
    max_s -= 8.0f;
    if (max_s < 1.0f)
        max_s = 1.0f;
    /* Keep glyph size independent from a larger square hit target. */
    float s = rec->content.glyph_size > 0.0f ? rec->content.glyph_size : rs->font_size * 1.55f;
    if (s > max_s)
        s = max_s;
    if (s < 1.0f)
        s = 1.0f;

    /* Background tint: transparent at rest, hover tint when hovered, and a
     * steady neutral tint when active — the universal "this is selected"
     * signal, identical for flat and rounded tiles. */
    float fill = active_surface ? 1.0f : rec->hover_t * 0.6f;
    if (fill > 0.001f) {
        flux_color bg =
            active_surface ? rs->bg_pressed : lensi_lerp_color(rs->bg, rs->bg_hover, fill);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = rounded ? rs->corner_radius : 0.0f});
    }

    float icon_x = (w - s) * 0.5f;
    float icon_y = (h - s) * 0.5f;

    /* Glyph: dimmed when disabled, accent only for the checked toggle (a
     * glyph-swap content variant), plain foreground otherwise. */
    flux_color glyph = disabled ? rs->disabled : rec->content.accent_checked ? rs->accent : rs->fg;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){
                            .kind = LENS_DRAW_ICON,
                            .rel = {icon_x, icon_y, s, s},
                            .color = glyph,
                            /* Same 2/24 stroke ratio as bare lens_icon, so
                             * buttons and inline icons read as one set. */
                            .width = 2.0f * (s / 24.0f),
                            .icon_id = rec->content.icon,
                        });

    if (rec->content.badge && rec->content.badge[0]) {
        float badge_size = s * 0.42f;
        if (badge_size < 8.0f)
            badge_size = 8.0f;
        lens_text_metrics bm = lensi_text_measure_label(ui, rec->content.badge, badge_size, 650.0f);
        float badge_x = icon_x + s - bm.width * 0.62f;
        float badge_y = icon_y - bm.height * 0.30f;
        if (badge_x + bm.width > w)
            badge_x = w - bm.width;
        if (badge_y < 0.0f)
            badge_y = 0.0f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {badge_x, badge_y, 0, 0},
                                            .color = glyph,
                                            .text = rec->content.badge,
                                            .text_size = badge_size,
                                            .text_weight = 650.0f});
    }
}
