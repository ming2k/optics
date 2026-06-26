/* split.c — resizable two-pane container with a draggable handle (ADR-0018).
 *
 * A split is a container whose two panes are sized by a persisted ratio,
 * separated by a grabbable divider. Dragging the divider mutates the
 * ratio; the layout pass redistributes the space. The drag state machine
 * mirrors the scroll thumb (ADR-0006): active_id capture + a dragging flag
 * + a delta from the press anchor. */

#include "../internal.h"
#include <stdio.h>

typedef struct {
    float ratio;                 /* 0..1 fraction of main axis for pane 1 */
    float min_first, min_second; /* logical-px floors */
    float thickness;             /* handle strip thickness */
    bool dragging;
    bool pane_open; /* a pane is currently open (for auto-close) */
    float drag_start_ratio;
    float drag_start_pos; /* cursor main-axis position at drag start */
} lens_split_state;

/* ------------------------------------------------------------------ */
/*  Split container                                                    */
/* ------------------------------------------------------------------ */

bool lens_split_begin(lens *ui, const char *id, lens_split_direction dir,
                      const lens_split_opts *opts) {
    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return false;
    lensi_link_child(ui, n);

    float seed_ratio = (opts && opts->ratio > 0) ? opts->ratio : 0.5f;
    if (seed_ratio > 1.0f)
        seed_ratio = 1.0f;

    n->is_container = true;
    n->axis = (dir == LENS_SPLIT_VERTICAL) ? LENS_ROW : LENS_COLUMN;
    n->gap = 0.0f;
    n->pad = 0.0f;
    n->cross = LENS_STRETCH;

    lens_split_state *st = lens_node_state(n, sizeof *st);
    if (st) {
        if (st->ratio <= 0.0f)
            st->ratio = seed_ratio;
        if (st->thickness <= 0.0f)
            st->thickness = (opts && opts->thickness > 0) ? opts->thickness : 6.0f;
        st->min_first = (opts && opts->min_first > 0) ? opts->min_first : 0;
        st->min_second = (opts && opts->min_second > 0) ? opts->min_second : 0;
        st->pane_open = false; /* reset for this frame's pane sequence */
    }

    /* Push as the current container so the panes link into it. */
    lensi_open_container_push(ui, n);
    return true;
}

/* Open a child container for one pane. The first call opens pane 1; the
 * second auto-closes pane 1 and opens pane 2. The caller fills each pane
 * between calls. lens_split_end closes pane 2 and the split. */
bool lens_split_pane(lens *ui) {
    lens_node *split = lensi_open_container(ui);

    /* If the open container is a pane (not the split itself), this is the
     * second pane: close the first so the new pane is a sibling. */
    lens_split_state *sst = split ? lens_node_state(split, sizeof *sst) : NULL;
    if (sst && sst->pane_open) {
        lensi_open_container_pop(ui); /* close pane 1 → back to the split */
        split = lensi_open_container(ui);
    }

    lens_id cid = lensi_gen_container_id(ui, "pane");
    lens_node *n = lensi_store_touch(ui, cid);
    if (!n)
        return false;
    n->is_container = true;
    n->axis = LENS_COLUMN;
    n->gap = ui->theme.gap;
    n->pad = 0.0f;
    n->cross = LENS_STRETCH;
    lensi_link_child(ui, n);
    lensi_open_container_push(ui, n);

    if (sst)
        sst->pane_open = true;
    return true;
}

void lens_split_end(lens *ui) {
    const lens_theme *t = &ui->theme;

    /* Close the second pane (the current open container). */
    lensi_open_container_pop(ui);

    if (ui->cont_top == 0)
        return;
    lens_node *split = ui->cont_stack[ui->cont_top - 1];
    lens_split_state *st = lens_node_state(split, sizeof *st);
    if (!st) {
        lensi_open_container_pop(ui);
        return;
    }

    bool vertical = (split->axis == LENS_ROW);
    const int L = LENS_MOUSE_LEFT;
    float cur_main = vertical ? ui->input.cursor.x : ui->input.cursor.y;

    /* Available main-axis length from last frame. */
    float avail = 0.0f;
    if (split->has_prev)
        avail = vertical ? split->prev_rect.w : split->prev_rect.h;
    float thick = st->thickness;
    float usable = avail - thick;
    if (usable < 0)
        usable = 0;

    /* Divider position in last frame's space. */
    float origin = vertical ? split->prev_rect.x : split->prev_rect.y;
    float div_pos = origin + usable * st->ratio;
    float cross_lo = vertical ? split->prev_rect.y : split->prev_rect.x;
    float cross_hi = vertical ? split->prev_rect.y + split->prev_rect.h
                              : split->prev_rect.x + split->prev_rect.w;
    float cur_cross = vertical ? ui->input.cursor.y : ui->input.cursor.x;
    bool over_handle = split->has_prev && cur_main >= div_pos - thick * 0.5f &&
                       cur_main <= div_pos + thick * 0.5f && cur_cross >= cross_lo &&
                       cur_cross <= cross_hi;

    /* Drag state machine (scroll-thumb model). */
    if (ui->active_id == split->id) {
        if (ui->input.mouse_down[L] && st->dragging) {
            float d = cur_main - st->drag_start_pos;
            float new_ratio = st->drag_start_ratio + (usable > 0 ? d / usable : 0);
            if (st->min_first > 0 && new_ratio * usable < st->min_first)
                new_ratio = st->min_first / usable;
            if (st->min_second > 0 && (1.0f - new_ratio) * usable < st->min_second)
                new_ratio = 1.0f - st->min_second / usable;
            if (new_ratio < 0)
                new_ratio = 0;
            if (new_ratio > 1)
                new_ratio = 1;
            st->ratio = new_ratio;
        }
        if (ui->input.mouse_released[L]) {
            st->dragging = false;
            ui->active_id = 0;
        }
    } else if (over_handle && ui->input.mouse_pressed[L]) {
        ui->active_id = split->id;
        st->dragging = true;
        st->drag_start_ratio = st->ratio;
        st->drag_start_pos = cur_main;
    }

    /* Size the two panes via fixed main-axis dimension. */
    float first_len = usable * st->ratio;
    float second_len = usable - first_len;
    lens_node *pane1 = split->first_child;
    lens_node *pane2 = split->last_child;
    if (pane1) {
        if (vertical)
            pane1->fixed_w = first_len;
        else
            pane1->fixed_h = first_len;
    }
    if (pane2 && pane2 != pane1) {
        if (vertical)
            pane2->fixed_w = second_len;
        else
            pane2->fixed_h = second_len;
    }

    /* Draw the handle strip, positioned at the divider (prev_rect space). */
    bool hov = over_handle || st->dragging;
    flux_color hc = hov ? t->color_active : t->color_border;
    if (split->has_prev) {
        if (vertical) {
            float hx = first_len - thick * 0.5f;
            lensi_drawlist_push(ui, split,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {hx, 0, thick, split->prev_rect.h},
                                                .color = hc,
                                                .radius = thick * 0.5f});
        } else {
            float hy = first_len - thick * 0.5f;
            lensi_drawlist_push(ui, split,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, hy, split->prev_rect.w, thick},
                                                .color = hc,
                                                .radius = thick * 0.5f});
        }
    }

    /* a11y: the split handle is an adjustable (slider). */
    char val[16];
    snprintf(val, sizeof val, "%.2f", st->ratio);
    lensi_node_semantics(ui, split, LENS_ROLE_SLIDER, "split", val, 0);

    /* Record a response so the host can set a resize cursor. */
    ui->last_response = (lens_response){
        .id = split->id, .rect = split->prev_rect, .hovered = hov, .pressed = st->dragging};

    lensi_open_container_pop(ui); /* pop the split container */
}

/* Read the current ratio (for persistence across restarts). */
float lens_split_ratio(const lens *ui, const char *id) {
    if (!ui || !id)
        return 0.5f;
    /* gen_widget_id mutates the seq counter; re-derive without the side
     * effect via lens_current_id. */
    lens_id lid = lens_current_id(ui, id);
    const lens_node *n = lensi_store_find((lens *)ui, lid);
    if (!n)
        return 0.5f;
    const lens_split_state *st = n->state;
    if (st && n->state_bytes >= sizeof *st)
        return st->ratio;
    return 0.5f;
}
