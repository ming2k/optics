/* segmented.c — segmented control pattern implementation. */

#include "../internal.h"
#include <optics/ui/patterns.h>

#include <stdio.h>

bool optics_ui_segmented_control(optics_ui *ui, const optics_ui_segmented_opts *opts) {
    if (!opts)
        return false;
    const char *id_str = opts->id;
    const optics_ui_segmented_item *items = opts->items;
    const uint32_t item_count = opts->item_count;
    uint32_t *selected_index = opts->selected_index;

    if (!ui || !items || item_count == 0 || !selected_index)
        return false;

    optics_ui_push_id(ui, id_str ? id_str : "segmented");

    float h =
        (opts && opts->height > 0.0f) ? opts->height : ((opts && opts->compact) ? 30.0f : 34.0f);
    float radius = (opts && opts->compact) ? 6.0f : 8.0f;
    float border_w = ui->theme.border_width;

    /* Base theme colors */
    optics_gfx_color bg = (border_w > 0.0f) ? ui->theme.color_bg : ui->theme.color_surface_sunken;
    optics_gfx_color border_color = ui->theme.color_border;

    /* Outer container */
    optics_ui_layout_opts row_opts = {
        .box =
            {
                .height = h,
            },
        .gap = 2.0f,
        .pad = 2.0f,
        .cross = OPTICS_UI_STRETCH,
        .radius = radius,
        .border_width = border_w,
        .bg = bg,
        .border = (border_w > 0.0f) ? border_color : 0,
    };

    optics_ui_row_begin(ui, &row_opts);

    bool changed = false;
    uint32_t curr_sel = *selected_index;

    for (uint32_t i = 0; i < item_count; i++) {
        const optics_ui_segmented_item *item = &items[i];
        bool is_selected = (i == curr_sel);
        char item_id[32];
        snprintf(item_id, sizeof(item_id), "seg_%u", i);
        float inner_radius = radius > 2.0f ? radius - 2.0f : radius;

        optics_ui_button_opts btn = {
            .box =
                {
                    .id = item_id,
                    .height = h - 4.0f,
                    .style =
                        {
                            .corner_radius = inner_radius,
                            .fields = OPTICS_UI_STYLE_CORNER_RADIUS,
                        },
                },
            .label = item->label,
            .icon = item->icon,
            .variant = is_selected ? OPTICS_UI_BUTTON_PRIMARY : OPTICS_UI_BUTTON_SUBTLE,
            .active = is_selected,
        };

        if (opts && opts->min_item_width > 0.0f) {
            btn.box.min_width = opts->min_item_width;
        }

        optics_ui_response resp = optics_ui_button(ui, &btn);

        if (!item->disabled && resp.clicked && !is_selected) {
            *selected_index = i;
            changed = true;
        }
    }

    optics_ui_row_end(ui);
    optics_ui_pop_id(ui);

    return changed;
}
