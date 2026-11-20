/* split_pane.c — interactive split divider pattern implementation. */

#include "../internal.h"
#include <optics/ui/patterns.h>

bool optics_ui_split_handle_v(optics_ui *ui, const char *id_str, float *split_offset,
                              const optics_ui_split_opts *opts) {
    if (!ui || !split_offset)
        return false;

    float min_s = (opts && opts->min_size > 0.0f) ? opts->min_size : 120.0f;
    float max_s = (opts && opts->max_size > min_s) ? opts->max_size : 600.0f;
    float handle_w = (opts && opts->handle_width > 0.0f) ? opts->handle_width : 2.0f;

    optics_ui_push_id(ui, id_str ? id_str : "split_v");

    optics_ui_separator_opts sep_opts = {
        .box =
            {
                .id = "split_bar",
                .width = handle_w,
            },
        .axis = OPTICS_UI_COLUMN,
        .thickness = handle_w,
    };

    optics_ui_response resp = optics_ui_separator(ui, &sep_opts);

    /* Check dragging: when mouse is held on divider */
    bool changed = false;
    if (resp.pressed) {
        float new_offset = ui->input.cursor.x;
        if (new_offset < min_s)
            new_offset = min_s;
        if (new_offset > max_s)
            new_offset = max_s;
        if (new_offset != *split_offset) {
            *split_offset = new_offset;
            changed = true;
        }
    }

    if (resp.hovered || resp.pressed) {
        ui->cursor_hint = OPTICS_UI_CURSOR_RESIZE_EW;
    }

    optics_ui_pop_id(ui);
    return changed;
}
