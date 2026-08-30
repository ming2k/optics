/* segmented.c — segmented control pattern implementation. */

#include "../internal.h"
#include "../../include/lens/patterns.h"

#include <stdio.h>

bool lens_segmented_control(lens *ui,
                             const char *id_str,
                             const lens_segmented_item *items,
                             uint32_t item_count,
                             uint32_t *selected_index,
                             const lens_segmented_opts *opts) {
    if (!ui || !items || item_count == 0 || !selected_index)
        return false;

    lens_push_id(ui, id_str ? id_str : "segmented");

    float h = (opts && opts->height > 0.0f)
                  ? opts->height
                  : ((opts && opts->compact) ? 30.0f : 34.0f);
    float radius = (opts && opts->compact) ? 6.0f : 8.0f;
    float border_w = 1.0f;

    /* Base theme colors */
    flux_color bg = ui->theme.color_bg;
    flux_color border_color = ui->theme.color_border;

    /* Outer container */
    lens_layout_opts row_opts = {
        .box = {
            .height = h,
        },
        .gap = 2.0f,
        .pad = 2.0f,
        .cross = LENS_STRETCH,
        .radius = radius,
        .border_width = border_w,
        .bg = bg,
        .border = border_color,
    };

    lens_row_begin(ui, &row_opts);

    bool changed = false;
    uint32_t curr_sel = *selected_index;

    for (uint32_t i = 0; i < item_count; i++) {
        const lens_segmented_item *item = &items[i];
        bool is_selected = (i == curr_sel);
        char item_id[32];
        snprintf(item_id, sizeof(item_id), "seg_%u", i);

        lens_button_opts btn = {
            .box = {
                .id = item_id,
                .height = h - 4.0f,
            },
            .label = item->label,
            .icon = item->icon,
            .variant = is_selected ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            .active = is_selected,
        };

        if (opts && opts->min_item_width > 0.0f) {
            btn.box.min_width = opts->min_item_width;
        }

        lens_response resp = lens_button(ui, &btn);

        if (!item->disabled && resp.clicked && !is_selected) {
            *selected_index = i;
            changed = true;
        }
    }

    lens_row_end(ui);
    lens_pop_id(ui);

    return changed;
}
