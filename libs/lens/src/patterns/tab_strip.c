/* tab_strip.c — tab strip interaction pattern. */

#include "../../include/lens/patterns.h"
#include "../internal.h"

#include <stdio.h>

#define TAB_DRAG_THRESHOLD_SQ 196.0f /* 14px Euclidean movement threshold (ADR-0086 parity) */

/* Persistent per-strip drag state, stored via lens_node_state on a retained
 * node keyed by the strip identity (ADR-0027: zeroed on first touch, lives
 * with the node). The host reorders its model on MOVE; between presses every
 * frame is rebuilt from the caller's tab array, so nothing here owns tab
 * identity — only the in-flight drag is remembered. */
typedef struct tab_strip_drag {
    bool active;      /* pointer captured and past the drag threshold */
    bool armed;       /* a press is held and may still cross the threshold */
    uint32_t from;    /* index of the tab being dragged */
    uint32_t hover;   /* current predicted insertion slot (0..tab_count-1) */
    uint32_t count;   /* tab_count captured at press, to detect model drift */
    flux_point press; /* pointer position at press */
} tab_strip_drag;

lens_tab_action lens_tab_strip(lens *ui, const char *id_str, const lens_tab_item *tabs,
                               uint32_t tab_count, uint32_t active_index,
                               const lens_tab_strip_opts *opts) {
    lens_tab_action action = {.kind = LENS_TAB_ACTION_NONE, .index = 0, .to = 0};
    if (!ui || !tabs || tab_count == 0)
        return action;

    lens_push_id(ui, id_str ? id_str : "tab_strip");

    float h = (opts && opts->height > 0.0f) ? opts->height : 36.0f;
    float tab_h = h - 6.0f;
    float min_w = (opts && opts->min_tab_width > 0.0f) ? opts->min_tab_width : 80.0f;
    float max_w = (opts && opts->max_tab_width > 0.0f) ? opts->max_tab_width : 200.0f;

    lens_layout_opts strip_opts = {
        .box =
            {
                .height = h,
            },
        .gap = 4.0f,
        .pad = 3.0f,
        .cross = LENS_CENTER,
        .bg = ui->theme.color_bg,
    };

    lens_row_begin(ui, &strip_opts);

    /* Drag state hangs on a stable id derived from the strip identity so it
     * survives frame-to-frame rebuilds of the tab list. */
    lens_id drag_state_id = lens_current_id(ui, "drag_state");
    lens_node *drag_node = lensi_store_touch(ui, drag_state_id);
    tab_strip_drag *drag =
        drag_node ? (tab_strip_drag *)lens_node_state(drag_node, sizeof(tab_strip_drag)) : NULL;

    /* Model drift (close/new/reorder between press and release) invalidates
     * the armed drag: drop it rather than move a stale index. */
    if (drag && drag->count != tab_count)
        drag->active = false;

    /* Predicted insertion slot scan state (filled while walking the tabs). */
    bool slot_decided = false;
    uint32_t pre_slot = 0;

    for (uint32_t i = 0; i < tab_count; i++) {
        const lens_tab_item *tab = &tabs[i];
        bool is_active = (i == active_index);
        char tab_id[32];
        snprintf(tab_id, sizeof(tab_id), "tab_%u", i);

        lens_selectable_opts sel = {
            .box =
                {
                    .id = tab_id,
                    .height = tab_h,
                    .min_width = min_w,
                    .max_width = max_w,
                },
            .label = tab->title,
            .selected = is_active,
            .icon = tab->icon,
        };

        lens_response resp = lens_selectable(ui, &sel);
        if (resp.clicked && !is_active) {
            action.kind = LENS_TAB_ACTION_SELECT;
            action.index = i;
        }
        /* Press landing on the tab must not turn into a CSD window drag. */
        if (resp.pressed && ui->input.mouse_pressed[0])
            action.pressed_on_tab = true;

        /* --- Drag-to-reorder state machine ------------------------------- */
        if (drag) {
            /* Arm: press captured on this tab records the drag candidate.
             * The candidate belongs to the gesture that pressed it and dies
             * with the button release — so a later gesture can never inherit
             * a stale `from` and cross the threshold from the old press. */
            if (ui->input.mouse_pressed[0] && resp.pressed) {
                drag->armed = true;
                drag->active = false;
                drag->from = i;
                drag->hover = i;
                drag->count = tab_count;
                drag->press = ui->input.cursor;
            }

            bool mine = drag->armed && drag->from == i && drag->count == tab_count;
            if (mine && !drag->active && ui->input.mouse_down[0]) {
                float dx = (float)(ui->input.cursor.x - drag->press.x);
                float dy = (float)(ui->input.cursor.y - drag->press.y);
                if (dx * dx + dy * dy >= TAB_DRAG_THRESHOLD_SQ)
                    drag->active = true; /* preview slot starts at the origin */
            }

            if (drag->active && drag->count == tab_count && !slot_decided && i != drag->from) {
                /* Predicted insertion slot from last-frame rects: the first
                 * tab whose midpoint sits right of the cursor marks the slot
                 * boundary. Tab rects ascend in x, so the first hit wins;
                 * if none, the slot is past the last tab (append). The slot
                 * is reported in post-removal coordinates (remove(from)
                 * first, then insert(to)), so the host applies the move
                 * without index gymnastics. */
                flux_rect r = resp.rect;
                float mid = r.x + r.w * 0.5f;
                if (ui->input.cursor.x < mid) {
                    slot_decided = true;
                    pre_slot = i;
                }
            }

            if (mine && drag->active)
                ui->cursor_hint = LENS_CURSOR_POINTER;
        }

        /* Optional close button */
        if (tab->closable) {
            char close_id[32];
            snprintf(close_id, sizeof(close_id), "close_%u", i);
            lens_button_opts close_btn = {
                .box =
                    {
                        .id = close_id,
                        .width = 18.0f,
                        .height = 18.0f,
                    },
                .icon = (opts && opts->close_icon != LENS_ICON_INVALID) ? opts->close_icon
                                                                        : LENS_ICON_INVALID,
                .label = (opts && opts->close_icon != LENS_ICON_INVALID) ? NULL : "×",
                .variant = LENS_BUTTON_SUBTLE,
            };

            lens_response close_resp = lens_button(ui, &close_btn);
            if (close_resp.clicked) {
                action.kind = LENS_TAB_ACTION_CLOSE;
                action.index = i;
            }
            if (close_resp.pressed && ui->input.mouse_pressed[0])
                action.pressed_on_tab = true;
        }
    }

    /* Resolve the predicted slot (post-removal coordinates) and finish the
     * drag on release. Done after the loop so the scan sees every tab. */
    if (drag && drag->active && drag->count == tab_count) {
        uint32_t slot = slot_decided ? pre_slot : tab_count - 1;
        drag->hover = slot;
        if (ui->input.mouse_released[0]) {
            /* A tab cannot move onto its own removal slot. */
            if (slot != drag->from && slot < tab_count) {
                action.kind = LENS_TAB_ACTION_MOVE;
                action.index = drag->from;
                action.to = slot;
            }
            drag->active = false;
        }
    }
    /* Release ends the gesture itself: even without a drag, the armed
     * candidate must not survive into the next press. */
    if (drag && ui->input.mouse_released[0]) {
        drag->armed = false;
        drag->active = false;
    }

    /* Trailing New Tab Button */
    if (opts && opts->show_new_button) {
        lens_button_opts new_btn = {
            .box =
                {
                    .id = "btn_new_tab",
                    .width = tab_h,
                    .height = tab_h,
                },
            .icon = opts->new_icon,
            .label = (opts->new_icon != LENS_ICON_INVALID) ? NULL : "+",
            .variant = LENS_BUTTON_SUBTLE,
        };

        lens_response new_resp = lens_button(ui, &new_btn);
        if (new_resp.clicked) {
            action.kind = LENS_TAB_ACTION_NEW;
        }
        if (new_resp.pressed && ui->input.mouse_pressed[0])
            action.pressed_on_tab = true;
    }

    lens_row_end(ui); /* end strip container */
    lens_pop_id(ui);

    return action;
}
