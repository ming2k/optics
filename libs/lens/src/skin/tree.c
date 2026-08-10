/* skin/tree.c — default tree-row skin (ADR-0059): hover/focus row tint,
 * disclosure chevron (branch) or dot (leaf), indented label — moved
 * verbatim from widgets/tree.c. The open state, the body containers, and
 * the indentation wrappers stay in the widget (layout behaviour). */

#include "../internal.h"

void lensi_skin_tree(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;
    float h = rec->bounds.h;
    float tm_h = rec->content.text.height;
    float icon = tm_h * 0.7f;

    /* Row background tint on hover / focus (matches the menu item style). */
    if (rec->hover_t > 0.01f && !disabled) {
        flux_color bg = lensi_lerp_color(rs->bg_hover, rs->bg_pressed, rec->hover_t * 0.5f);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = rs->corner_radius * 0.5f});
    }

    /* Disclosure glyph: chevron for branches, dot for leaves. */
    float icon_y = (h - icon) * 0.5f;
    flux_color fg = disabled ? rs->disabled : rs->fg;
    if (rec->content.leaf) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {0, icon_y, icon, icon},
                                            .color = rs->disabled,
                                            .width = 1.6f * (icon / 24.0f),
                                            .icon_id = LENS_ICON_CIRCLE});
    } else {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {0, icon_y, icon, icon},
                                            .color = fg,
                                            .width = 1.8f * (icon / 24.0f),
                                            .icon_id = rec->content.icon});
    }

    /* Label, indented past the glyph. */
    float label_x = icon + rs->padding * 0.5f;
    float text_y = (h - tm_h) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {label_x, text_y, 0, 0},
                                        .color = fg,
                                        .text = rec->content.label,
                                        .text_size = rs->font_size,
                                        .text_weight = 400.0f});
}
