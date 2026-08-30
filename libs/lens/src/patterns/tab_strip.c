/* tab_strip.c — tab strip interaction pattern. */

#include "../internal.h"
#include "../../include/lens/patterns.h"

#include <stdio.h>

lens_tab_action lens_tab_strip(lens *ui,
                               const char *id_str,
                               const lens_tab_item *tabs,
                               uint32_t tab_count,
                               uint32_t active_index,
                               const lens_tab_strip_opts *opts) {
    lens_tab_action action = { .kind = LENS_TAB_ACTION_NONE, .index = 0 };
    if (!ui || !tabs || tab_count == 0)
        return action;

    lens_push_id(ui, id_str ? id_str : "tab_strip");

    float h = (opts && opts->height > 0.0f) ? opts->height : 36.0f;
    float tab_h = h - 6.0f;
    float min_w = (opts && opts->min_tab_width > 0.0f) ? opts->min_tab_width : 80.0f;
    float max_w = (opts && opts->max_tab_width > 0.0f) ? opts->max_tab_width : 200.0f;

    lens_layout_opts strip_opts = {
        .box = {
            .height = h,
        },
        .gap = 4.0f,
        .pad = 3.0f,
        .cross = LENS_CENTER,
        .bg = ui->theme.color_bg,
    };

    lens_row_begin(ui, &strip_opts);

    for (uint32_t i = 0; i < tab_count; i++) {
        const lens_tab_item *tab = &tabs[i];
        bool is_active = (i == active_index);
        char tab_id[32];
        snprintf(tab_id, sizeof(tab_id), "tab_%u", i);

        lens_selectable_opts sel = {
            .box = {
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

        /* Optional close button */
        if (tab->closable) {
            char close_id[32];
            snprintf(close_id, sizeof(close_id), "close_%u", i);
            lens_button_opts close_btn = {
                .box = {
                    .id = close_id,
                    .width = 18.0f,
                    .height = 18.0f,
                },
                .icon = (opts && opts->close_icon != LENS_ICON_INVALID) ? opts->close_icon : LENS_ICON_INVALID,
                .label = (opts && opts->close_icon != LENS_ICON_INVALID) ? NULL : "×",
                .variant = LENS_BUTTON_SUBTLE,
            };

            lens_response close_resp = lens_button(ui, &close_btn);
            if (close_resp.clicked) {
                action.kind = LENS_TAB_ACTION_CLOSE;
                action.index = i;
            }
        }
    }

    /* Trailing New Tab Button */
    if (opts && opts->show_new_button) {
        lens_button_opts new_btn = {
            .box = {
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
    }

    lens_row_end(ui); /* end strip container */
    lens_pop_id(ui);

    return action;
}
