/* scroll.c — scrollable container (ADR-0031). */

#include "../internal.h"

#define OPTICS_UI_SCROLL_SPEED 40.0f

bool optics_ui_scroll_begin(optics_ui *ui, const optics_ui_scroll_opts *opts) {
    optics_ui_scroll_opts default_opts = {};
    if (!opts)
        opts = &default_opts;

    optics_ui__apply_box(ui, opts->box);
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_style = (optics_ui_style){};
    const optics_ui_theme *t = &ui->theme;
    const char *id_str = opts->box.id ? opts->box.id : "##scroll";
    optics_ui_id fid = optics_ui__gen_widget_id(ui, id_str);
    optics_ui_node *n = optics_ui__store_touch(ui, fid);
    if (!n)
        return false;
    optics_ui__link_child(ui, n);
    optics_ui__node_box(ui, n, &opts->box);

    n->is_container = true;
    n->is_scroll = true;
    n->axis = OPTICS_UI_COLUMN;
    n->gap = t->gap;
    n->pad = 0.0f;
    n->cross = OPTICS_UI_STRETCH;

    if (opts->box.max_width > 0.0f)
        n->max_w = opts->box.max_width;
    if (opts->box.max_height > 0.0f)
        n->max_h = opts->box.max_height;

    bool occluded = n->has_prev && (optics_ui__widget_occluded(ui, n) ||
                                    optics_ui__snapshot_point_clipped(n, ui->input.cursor));
    if (n->has_prev && !occluded && optics_ui__point_in(ui->input.cursor, n->prev_rect)) {
        ui->scroll_hot_id = n->id;
    }

    optics_ui_scroll_state *ss =
        (optics_ui_scroll_state *)optics_ui_node_state(n, sizeof(optics_ui_scroll_state));
    if (ss) {
        optics_ui_scroll_geometry presented = {};
        bool have_bar = optics_ui__snapshot_scroll_geometry(n, &presented) && presented.thumb.h > 0;
        optics_gfx_rect thumb_rect = presented.thumb;
        optics_gfx_rect track_rect = presented.track;
        if (have_bar) {
            const int L = OPTICS_UI_MOUSE_LEFT;
            if (ui->active_id == n->id) {
                if (ui->input.mouse_down[L] && ss->dragging) {
                    float dy = ui->input.cursor.y - ss->drag_start_y;
                    float dscroll = dy * ss->drag_ratio;
                    ss->offset_y = ss->drag_start_offset + dscroll;
                } else if (!ui->input.mouse_down[L]) {
                    ui->active_id = 0;
                    ss->dragging = false;
                }
            } else if (!occluded && optics_ui__point_in(ui->input.cursor, thumb_rect) &&
                       ui->input.mouse_pressed[L]) {
                ui->active_id = n->id;
                ss->dragging = true;
                ss->drag_start_offset = presented.offset_y;
                ss->drag_ratio =
                    presented.track_len > 0 ? presented.scroll_range / presented.track_len : 0;
                ss->drag_start_y = ui->input.cursor.y;
                ui->input.mouse_pressed[L] = false;
            } else if (!occluded && optics_ui__point_in(ui->input.cursor, track_rect) &&
                       ui->input.mouse_pressed[L]) {
                float page = track_rect.h * 0.9f;
                if (ui->input.cursor.y < thumb_rect.y)
                    ss->offset_y -= page;
                else
                    ss->offset_y += page;
                ui->input.mouse_pressed[L] = false;
            }
        }

        ss->hovering = ss->dragging ||
                       (have_bar && !occluded && optics_ui__point_in(ui->input.cursor, track_rect));

        n->scroll_x = ss->offset_x;
        n->scroll_y = ss->offset_y;
    }

    optics_ui__node_semantics(ui, n, OPTICS_UI_ROLE_SCROLLAREA, nullptr, nullptr, 0);
    optics_ui__open_container_push(ui, n);
    return true;
}

void optics_ui_scroll_end(optics_ui *ui) {
    optics_ui_node *n = optics_ui__open_container(ui);
    if (n && n->is_scroll && ui->scroll_hot_id == n->id) {
        optics_ui_scroll_state *ss =
            (optics_ui_scroll_state *)optics_ui_node_state(n, sizeof(optics_ui_scroll_state));
        if (ss) {
            ss->offset_y -= ui->input.scroll_y * OPTICS_UI_SCROLL_SPEED + ui->input.scroll_pixels_y;
            ss->offset_x -= ui->input.scroll_x * OPTICS_UI_SCROLL_SPEED + ui->input.scroll_pixels_x;
            n->scroll_y = ss->offset_y;
            n->scroll_x = ss->offset_x;
        }
        ui->input.scroll_y = 0.0f;
        ui->input.scroll_x = 0.0f;
        ui->input.scroll_pixels_y = 0.0f;
        ui->input.scroll_pixels_x = 0.0f;
        ui->scroll_hot_id = 0;
    }
    optics_ui__open_container_pop(ui);
}

bool optics_ui_scroll_offset(const optics_ui *ui, const optics_ui_scroll_offset_opts *opts) {
    if (!ui || !opts || !opts->id)
        return false;
    optics_ui_id fid = optics_ui_current_id(ui, opts->id);
    optics_ui_node *n = optics_ui_find((optics_ui *)ui, fid);
    if (!n || !n->is_scroll)
        return false;
    optics_ui_scroll_state *ss =
        (optics_ui_scroll_state *)optics_ui_node_state(n, sizeof(optics_ui_scroll_state));
    if (!ss)
        return false;
    if (opts->out_x)
        *opts->out_x = ss->offset_x;
    if (opts->out_y)
        *opts->out_y = ss->offset_y;
    return true;
}

void optics_ui_scroll_to(optics_ui *ui, const optics_ui_scroll_to_opts *opts) {
    if (!ui || !opts || !opts->id)
        return;
    optics_ui_id fid = optics_ui_current_id(ui, opts->id);
    optics_ui_node *n = optics_ui_find(ui, fid);
    if (!n || !n->is_scroll)
        return;
    optics_ui_scroll_state *ss =
        (optics_ui_scroll_state *)optics_ui_node_state(n, sizeof(optics_ui_scroll_state));
    if (ss) {
        ss->offset_x = opts->x;
        ss->offset_y = opts->y;
        n->scroll_x = opts->x;
        n->scroll_y = opts->y;
    }
}
