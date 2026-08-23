/* skin/tabs.c — default tabs skin (ADR-0059/0061). The neutral default:
 * per-tab hover fill + label, and a STATIC selection indicator — resolved
 * accent, fixed 3px thickness, zero animation, so reduced-motion is
 * trivially satisfied. An animated or connected presentation is
 * caller-owned flavor behind the same seam (see
 * examples/showcase/tabs_spring_skin.c for the spring recipe).
 *
 * Geometry note: the skin runs at lens_tabs_end, before layout solves the
 * strip, so indicator targets derive from the tabs' last_bounds (one-frame
 * latency, ADR-0029) relative to the FIRST tab — child rectangles are
 * stable relative to one another even while an ancestor is moving, which
 * avoids applying screen-space offsets twice at replay. */

#include "../internal.h"

#define LENSI_TABS_INDICATOR_THICKNESS 3.0f

void lensi_skin_tabs(lens *ui, lens_node *strip, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;

    /* Per-tab chrome + label, pushed onto each tab node itself (node-local
     * rects resolve against the tab's final box at render time — no
     * build-time position needed). */
    lens_node *child = strip->first_child;
    for (int i = 0; i < c->tab_count && child; i++, child = child->next_sibling) {
        const lens_tab_item *tab = &c->tabs[i];
        bool disabled = (tab->state & LENS_STATE_DISABLED) != 0;
        bool focused = (tab->state & LENS_STATE_FOCUSED) != 0;

        if (!disabled && (tab->hover_t > 0.0f || focused)) {
            float emphasis = focused ? 1.0f : tab->hover_t;
            flux_color hover =
                lensi_lerp_color(flux_color_rgba(0, 0, 0, 0), rs->bg_hover, emphasis);
            lensi_drawlist_push(ui, child,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {2.0f, 1.0f, 0, 0},
                                                .color = hover,
                                                .radius = rs->corner_radius});
        }

        float text_y = (child->measured.y - tab->text.height) * 0.5f;
        if (text_y < 0.0f)
            text_y = 0.0f;
        lensi_drawlist_push(ui, child,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {rs->padding, text_y, -1.0f, 0},
                                            .color = disabled ? rs->disabled : rs->fg,
                                            .text = tab->label,
                                            /* rs->font_size arrives already
                                             * multiplied by ui->text_scale (style.c); the
                                             * matching measure site is tabs.c, which scales
                                             * the raw token itself via lensi_font_px */
                                            .text_size = rs->font_size});
    }

    /* Static selection indicator: an accent bar centred under the active
     * tab's label, drawn at the strip's bottom edge (LENS_DRAW_TAB_INDICATOR
     * resolves y from the strip's final box at replay). */
    if (c->tab_count > 0 && c->active_index >= 0 && c->active_index < c->tab_count) {
        const lens_tab_item *first = &c->tabs[0];
        const lens_tab_item *sel = &c->tabs[c->active_index];
        if (first->last_bounds.w > 0.0f && sel->last_bounds.w > 0.0f) {
            float indicator_padding = fmaxf(8.0f, rs->padding * 0.75f);
            float tab_left = strip->pad + sel->last_bounds.x - first->last_bounds.x;
            float tab_width = sel->last_bounds.w;
            float indicator_w = fminf(tab_width, sel->text.width + 2.0f * indicator_padding);
            indicator_w = fminf(fmaxf(indicator_w, LENSI_TABS_INDICATOR_THICKNESS), tab_width);
            float left = tab_left + (tab_width - indicator_w) * 0.5f;
            float right = left + indicator_w;

            float known_width = rec->last_bounds.w > 0.0f ? rec->last_bounds.w : strip->fixed_w;
            if (known_width > 0.0f) {
                left = fminf(fmaxf(left, 0.0f), known_width);
                right = fminf(fmaxf(right, left), known_width);
            }

            lensi_drawlist_push(
                ui, strip,
                (lens_draw_cmd){
                    .kind = LENS_DRAW_TAB_INDICATOR,
                    .rel = {left, 0, fmaxf(LENSI_TABS_INDICATOR_THICKNESS, right - left), 0},
                    .color = rs->accent,
                    .width = LENSI_TABS_INDICATOR_THICKNESS});
        }
    }
}
