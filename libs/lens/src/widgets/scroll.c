/* scroll.c — scrollable container (ADR-0031). */

#include "../internal.h"

#define LENS_SCROLL_SPEED 40.0f

void lens_scroll_begin(lens *ui, const char *id) {
    const lens_theme *t = &ui->theme;
    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return;
    lensi_link_child(ui, n);

    n->is_container = true;
    n->is_scroll = true;
    n->axis = LENS_COLUMN;
    n->gap = t->gap;
    n->pad = 0.0f;
    n->cross = LENS_STRETCH;

    /* Wheel scroll is deferred to lens_scroll_end so the innermost hovered
       scroll container gets priority over outer ones. A floating layer
       above the area eclipses it (same rule as lensi_interact). */
    bool eclipsed = n->has_prev && lensi_widget_eclipsed(ui, n);
    if (n->has_prev && !eclipsed && lensi_point_in(ui->input.cursor, n->prev_rect)) {
        ui->scroll_hot_id = n->id;
    }

    lens_scroll_state *ss = (lens_scroll_state *)lens_node_state(n, sizeof(lens_scroll_state));
    if (ss) {
        /* scrollbar interaction (thumb drag + track click). All geometry is
         * theme-driven so consumers can size the bar to their chrome. */
        const float sb_w = t->scrollbar_width;
        bool have_bar = (ss->thumb_h > 0.0f && n->has_prev);
        flux_rect thumb_rect = {0, 0, 0, 0};
        flux_rect track_rect = {0, 0, 0, 0};
        float track_h = 0.0f;
        float sb_y = 0.0f;
        if (have_bar) {
            float sb_x = n->prev_rect.x + n->prev_rect.w - sb_w;
            sb_y = n->prev_rect.y + ss->thumb_y;
            thumb_rect = (flux_rect){sb_x, sb_y, sb_w, ss->thumb_h};
            track_h = ss->track_len + ss->thumb_h;
            track_rect = (flux_rect){sb_x, n->prev_rect.y, sb_w, track_h};

            const int L = LENS_MOUSE_LEFT;
            if (ui->active_id == n->id) {
                if (ui->input.mouse_down[L] && ss->dragging) {
                    float dy = ui->input.cursor.y - ss->drag_start_y;
                    float dscroll =
                        (ss->track_len > 0.0f) ? dy * ss->scroll_range / ss->track_len : 0.0f;
                    ss->offset_y = ss->drag_start_offset + dscroll;
                } else if (!ui->input.mouse_down[L]) {
                    ui->active_id = 0;
                    ss->dragging = false;
                }
            } else if (!eclipsed && lensi_point_in(ui->input.cursor, thumb_rect) &&
                       ui->input.mouse_pressed[L]) {
                ui->active_id = n->id;
                ss->dragging = true;
                ss->drag_start_offset = ss->offset_y;
                ss->drag_start_y = ui->input.cursor.y;
                ui->input.mouse_pressed[L] = false;
            } else if (!eclipsed && lensi_point_in(ui->input.cursor, track_rect) &&
                       ui->input.mouse_pressed[L]) {
                float page = track_h * 0.9f;
                if (ui->input.cursor.y < sb_y)
                    ss->offset_y -= page;
                else
                    ss->offset_y += page;
                ui->input.mouse_pressed[L] = false;
            }
        }

        /* hover state for draw styling: dragging wins, else cursor on track.
         * Persisted into ss so the post-layout draw pass (solve.c) can pick
         * the hovered thumb colour without recomputing hit geometry. */
        ss->hovering =
            ss->dragging || (have_bar && !eclipsed && lensi_point_in(ui->input.cursor, track_rect));

        n->scroll_x = ss->offset_x;
        n->scroll_y = ss->offset_y;
    }

    lensi_node_semantics(ui, n, LENS_ROLE_SCROLLAREA, NULL, NULL, 0);

    /* A scroll area is transparent by default — it inherits its parent's
     * surface (sidebar lists, file trees, etc. sit flush). A consumer that
     * wants a framed "card" wraps it in a container with a bg/border. */

    lensi_open_container_push(ui, n);
}

void lens_scroll_end(lens *ui) {
    lens_node *n = lensi_open_container(ui);
    if (n && n->is_scroll && ui->scroll_hot_id == n->id) {
        lens_scroll_state *ss = (lens_scroll_state *)lens_node_state(n, sizeof(lens_scroll_state));
        if (ss) {
            ss->offset_y -= ui->input.scroll_y * LENS_SCROLL_SPEED + ui->input.scroll_pixels_y;
            ss->offset_x -= ui->input.scroll_x * LENS_SCROLL_SPEED + ui->input.scroll_pixels_x;
            n->scroll_y = ss->offset_y;
            n->scroll_x = ss->offset_x;
        }
        ui->input.scroll_y = 0.0f;
        ui->input.scroll_x = 0.0f;
        ui->input.scroll_pixels_y = 0.0f;
        ui->input.scroll_pixels_x = 0.0f;
        ui->scroll_hot_id = 0;
    }
    lensi_open_container_pop(ui);
}

void lens_scroll_to(lens *ui, const char *id, float x, float y) {
    if (!ui || !id)
        return;
    lens_node *n = lensi_store_find(ui, lens_current_id(ui, id));
    if (!n || !n->is_scroll)
        return;
    lens_scroll_state *ss = (lens_scroll_state *)lens_node_state(n, sizeof(lens_scroll_state));
    if (!ss)
        return;
    ss->offset_x = fmaxf(0.0f, x);
    ss->offset_y = fmaxf(0.0f, y);
    n->scroll_x = ss->offset_x;
    n->scroll_y = ss->offset_y;
}
