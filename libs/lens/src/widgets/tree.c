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
    lens_style eff = lensi_style_effective(ui);

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);

    /* Header measure: label height plus a little vertical breathing room.
     * The chevron / dot leads the label by the resolved padding. */
    lens_style_resolved rs = lensi_style_resolve(&eff, t, 0);
    float label_size = rs.font_size;
    lens_text_metrics tm = lensi_text_measure_label(ui, label, label_size, 400.0f);
    float icon = tm.height * 0.7f;
    float h = tm.height + 4.0f;
    float w = tm.width + icon + rs.padding + LENS_TREE_INDENT;
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

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_TREE,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label,
                                    .text = tm,
                                    .icon = open ? LENS_ICON_CHEVRON_DOWN
                                                 : LENS_ICON_CHEVRON_RIGHT,
                                    .expanded = open,
                                    .leaf = leaf},
                    });

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
