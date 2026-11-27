/* skin/selectable.c — default selectable-row skin (ADR-0059).
 *
 * The pre-skin emit section of optics_ui_selectable (with or without an icon):
 * transparent at rest, subtle hover fill, steady active-surface tint when
 * selected. That tint is the neutral selection affordance; decorative
 * accent rails are flavor and belong to caller skins (ADR-0061). */

#include "../internal.h"

void optics_ui__skin_selectable(optics_ui *ui, optics_ui_node *n,
                                const optics_ui_widget_record *rec) {
    const optics_ui_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & OPTICS_UI_STATE_DISABLED) != 0;
    bool selected = (rec->state & OPTICS_UI_STATE_SELECTED) != 0;
    float h = rec->bounds.h;

    bool has_icon = optics_ui__icon_valid(ui, rec->content.icon);
    float icon_size =
        has_icon ? (rec->content.glyph_size > 0.0f ? rec->content.glyph_size : rs->icon_size)
                 : 0.0f;
    float icon_gap = has_icon ? (rs->gap > 0.0f ? rs->gap : 8.0f) : 0.0f;

    /* Background: transparent at rest. A selected row always uses the
     * active-surface colour. */
    float fill = rec->hover_t * 0.6f;
    if (selected || fill > 0.001f) {
        optics_gfx_color bg =
            selected ? rs->bg_pressed : optics_ui__lerp_color(0, rs->bg_hover, fill);
        if ((bg & 0xFF000000u) != 0) {
            optics_ui__drawlist_push(
                ui, n,
                (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                     .rel = {0, 0, 0, 0},
                                     .color = bg,
                                     .corner_radii =
                                         optics_gfx_corner_radii_uniform(rs->corner_radius)});
        }
    }

    float text_y = floorf((h - rec->content.text.height) * 0.5f);
    if (text_y < 0.0f)
        text_y = 0.0f;

    float x = floorf(rs->padding);
    optics_gfx_color fg = disabled ? rs->disabled : rs->fg;
    if (has_icon) {
        float icon_y = floorf((h - icon_size) * 0.5f);
        optics_ui__drawlist_push(ui, n,
                                 (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_ICON,
                                                      .rel = {x, icon_y, icon_size, icon_size},
                                                      .color = fg,
                                                      .width = 2.0f * (icon_size / 24.0f),
                                                      .icon_id = rec->content.icon});
        x += icon_size + icon_gap;
    }

    optics_ui__drawlist_push(
        ui, n,
        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_TEXT,
                             .rel = {x, text_y, 0, 0}, /* left-aligned (rel.w 0) */
                             .color = fg,
                             .text = rec->content.label,
                             .text_advance = rec->content.text.width,
                             .text_size = rs->font_size});
}
