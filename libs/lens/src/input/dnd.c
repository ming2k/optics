/* dnd.c — Immediate-mode Drag-and-Drop support for lens. (ADR-0086) */

#include "../internal.h"
#include <string.h>

#define LENS_DRAG_THRESHOLD_SQ 16.0f /* 4px Euclidean movement threshold */

bool lens_dnd_source(lens *ui, const lens_dnd_source_desc *desc) {
    if (!ui || !desc || !desc->id)
        return false;

    lens_node *n = lens_find(ui, desc->id);
    if (!n)
        return false;

    /* When pointer is pressed on this node, register potential drag origin */
    if (ui->active_id == desc->id && ui->input.mouse_pressed[0]) {
        ui->dnd_source.press_pos = ui->input.cursor;
        ui->dnd_source.source_id = desc->id;
        ui->dnd_source.active = false;
    }

    if (ui->active_id == desc->id && ui->input.mouse_down[0]) {
        if (!ui->dnd_source.active && ui->dnd_source.source_id == desc->id) {
            float dx = (float)(ui->input.cursor.x - ui->dnd_source.press_pos.x);
            float dy = (float)(ui->input.cursor.y - ui->dnd_source.press_pos.y);
            if ((dx * dx + dy * dy) >= LENS_DRAG_THRESHOLD_SQ) {
                ui->dnd_source.active = true;
                ui->dnd_source.drag_actions = desc->actions ? desc->actions : 1 /* COPY */;
                ui->dnd_source.preview_rect = desc->preview_rect;
                if (desc->text && desc->text_len) {
                    size_t cp = desc->text_len < (sizeof(ui->dnd_source.drag_text) - 1)
                                    ? desc->text_len
                                    : (sizeof(ui->dnd_source.drag_text) - 1);
                    memcpy(ui->dnd_source.drag_text, desc->text, cp);
                    ui->dnd_source.drag_text[cp] = '\0';
                    ui->dnd_source.drag_text_len = (uint32_t)cp;
                } else {
                    ui->dnd_source.drag_text[0] = '\0';
                    ui->dnd_source.drag_text_len = 0;
                }

                if (ui->dnd_host.start_drag) {
                    ui->dnd_host.start_drag(ui->dnd_source.drag_text, ui->dnd_source.drag_text_len,
                                            ui->dnd_source.drag_actions, ui->dnd_host.user);
                }
            }
        }
    }

    return ui->dnd_source.active && (ui->dnd_source.source_id == desc->id);
}

bool lens_dnd_drop_target(lens *ui, lens_id id, uint32_t accepted_actions,
                          lens_dnd_drop_info *out_info) {
    if (!ui || !id)
        return false;

    lens_node *n = lens_find(ui, id);
    if (!n)
        return false;

    flux_rect b = n->has_prev ? n->prev_rect : n->final_rect;
    flux_point p = ui->input.cursor;
    bool in_bounds = lensi_point_in(p, b);

    /* Check if a platform drop occurred inside this node's bounds */
    bool dropped_here = false;
    if (ui->dnd_drop.dropped && (ui->frame <= ui->dnd_drop.drop_frame + 1)) {
        flux_point dp = ui->dnd_drop.drop_pos;
        if (lensi_point_in(dp, b)) {
            dropped_here = true;
            in_bounds = true;
            p = dp;
        }
    }

    if (out_info) {
        out_info->is_hovered = in_bounds;
        out_info->is_dropped = dropped_here;
        out_info->drop_pos = (flux_point){p.x - b.x, p.y - b.y};
        out_info->action = accepted_actions;
    }

    return in_bounds;
}

void lens_deliver_drop(lens *ui, const char *payload, size_t len, flux_point pos) {
    if (!ui || !payload || !len)
        return;

    size_t cp =
        len < (sizeof(ui->dnd_drop.drop_buf) - 1) ? len : (sizeof(ui->dnd_drop.drop_buf) - 1);
    memcpy(ui->dnd_drop.drop_buf, payload, cp);
    ui->dnd_drop.drop_buf[cp] = '\0';
    ui->dnd_drop.drop_len = (uint32_t)cp;
    ui->dnd_drop.drop_pos = pos;
    ui->dnd_drop.drop_frame = ui->frame;
    ui->dnd_drop.dropped = true;
}

uint32_t lens_take_drop(lens *ui, char *dst, uint32_t cap) {
    if (!ui || !dst || cap == 0 || !ui->dnd_drop.drop_len)
        return 0;

    uint32_t n = ui->dnd_drop.drop_len < (cap - 1) ? ui->dnd_drop.drop_len : (cap - 1);
    memcpy(dst, ui->dnd_drop.drop_buf, n);
    dst[n] = '\0';
    ui->dnd_drop.drop_len = 0;
    ui->dnd_drop.dropped = false;
    return n;
}
