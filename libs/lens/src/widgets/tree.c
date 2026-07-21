/* tree.c — expandable tree view (ADR: lens tree widget).
 *
 * Implemented as a stack of nested disclosure rows on top of the same
 * retained-store machinery lens_collapsing uses. The widget:
 *   - renders a disclosure chevron (or a dot for leaves) at the current
 *     indentation level
 *   - renders the label
 *   - toggles its open state on click
 *   - when open, becomes a column container so children (typically more
 *     lens_tree_node calls) flow underneath with extra indentation
 *
 * Indentation is driven by the parent's id scope: lens_tree_node_end pops
 * one indirection level. The deepest the tree can nest is bounded only by
 * the store / arena; no fixed depth cap. */

#include "../internal.h"

/* Per-row retained state. */
typedef struct {
    bool open;
} lens_tree_state;

/* The indentation each nested level adds, in CSS pixels. Tuned so a deep
 * tree remains readable without overflowing typical sidebars. */
#define LENS_TREE_INDENT 14.0f

bool lens_tree_node(lens *ui, const char *label, bool leaf) {
    const lens_theme *t = &ui->theme;
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);

    /* Header measure: label height plus a little vertical breathing room.
     * The chevron / dot leads the label by t->padding. */
    float label_size = t->font_size;
    lens_text_metrics tm = lensi_text_measure_label(ui, label, label_size, 400.0f);
    float icon = tm.height * 0.7f;
    float h = tm.height + 4.0f;
    float w = tm.width + icon + t->padding + LENS_TREE_INDENT;
    if (n->fixed_w > 0)
        w = n->fixed_w;
    if (n->fixed_h > 0)
        h = n->fixed_h;
    n->measured = (flux_point){w, h};

    /* Hit-test only the header row (the body lives below). */
    flux_rect saved_rect = n->prev_rect;
    if (n->has_prev)
        n->prev_rect.h = h;
    lens_response r = lensi_interact(ui, n, true, disabled);
    n->prev_rect = saved_rect;

    lens_tree_state *st = lens_node_state(n, sizeof *st);
    if (!leaf && r.clicked && st)
        st->open = !st->open;
    bool open = !leaf && st && st->open;

    uint32_t sem_flags = (open ? LENS_A11Y_EXPANDED : 0) | (r.focused ? LENS_A11Y_FOCUSED : 0) |
                         (disabled ? LENS_A11Y_DISABLED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_DISCLOSURE, label, NULL, sem_flags);

    if (r.hovered)
        ui->cursor_hint = LENS_CURSOR_POINTER;
    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 18.f);

    /* Row background tint on hover / focus (matches the menu item style). */
    if (n->hover_t > 0.01f && !disabled) {
        flux_color bg = lensi_lerp_color(t->color_hover, t->color_active, n->hover_t * 0.5f);
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, 0},
                                            .color = bg,
                                            .radius = t->corner_radius * 0.5f});
    }

    /* Disclosure glyph: chevron for branches, dot for leaves. The icon
     * source is the same feather set lens_menu / lens_collapsing share. */
    float icon_y = (h - icon) * 0.5f;
    flux_color fg = disabled ? t->color_disabled : t->color_fg;
    if (leaf) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {0, icon_y, icon, icon},
                                            .color = t->color_disabled,
                                            .width = 1.6f * (icon / 24.0f),
                                            .icon_id = LENS_ICON_CIRCLE});
    } else {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_ICON,
                                            .rel = {0, icon_y, icon, icon},
                                            .color = fg,
                                            .width = 1.8f * (icon / 24.0f),
                                            .icon_id = open ? LENS_ICON_CHEVRON_DOWN
                                            : LENS_ICON_CHEVRON_RIGHT});
    }

    /* Label, indented past the glyph. */
    float label_x = icon + t->padding * 0.5f;
    float text_y = (h - tm.height) * 0.5f;
    if (text_y < 0.0f)
        text_y = 0.0f;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {label_x, text_y, 0, 0},
                                        .color = fg,
                                        .text = label,
                                        .text_size = label_size,
                                        .text_weight = 400.0f});

    ui->last_response = r;

    if (!open)
        return false;

    /* Open: become a column container. Children link in below this row;
     * lens_tree_node_end pops the container so siblings stay at the same
     * indentation. The first child reserves the header height as a spacer
     * so it does not overlap the row we just drew. */
    n->is_container = true;
    n->axis = LENS_COLUMN;
    n->gap = 0.0f;
    n->pad = 0.0f;
    lensi_open_container_push(ui, n);

    /* Header spacer (zero-width, header-tall). */
    lens_id sid = lensi_gen_widget_id(ui, "##tree_hdr");
    lens_node *spacer = lensi_store_touch(ui, sid);
    if (spacer) {
        lensi_link_child(ui, spacer);
        spacer->is_container = false;
        spacer->measured = (flux_point){0.0f, h};
        spacer->fixed_h = h;
    }

    /* Indentation wrapper: a column whose left padding shifts nested rows
     * right by LENS_TREE_INDENT. Nested lens_tree_node calls attach here
     * so their headers visually descend one level. */
    lens_id iid = lensi_gen_widget_id(ui, "##tree_body");
    lens_node *indent = lensi_store_touch(ui, iid);
    if (indent) {
        lensi_link_child(ui, indent);
        indent->is_container = true;
        indent->axis = LENS_COLUMN;
        indent->gap = 0.0f;
        indent->pad = LENS_TREE_INDENT;
        lensi_open_container_push(ui, indent);
    }
    return true;
}

void lens_tree_node_end(lens *ui) {
    /* Pop the indentation wrapper + the disclosure row itself. Two
     * lens_close calls match the two lensi_open_container_push calls
     * made when the open branch was entered. */
    lens_close(ui);
    lens_close(ui);
}

void lens_tree_node_set_open(lens *ui, const char *label, bool open) {
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    /* Same first-touch-only seeding contract as lens_collapsing_set_open:
     * only pre-render nodes accept the default. */
    if (n->has_prev)
        return;
    lens_tree_state *st = lens_node_state(n, sizeof *st);
    if (st)
        st->open = open;
}
