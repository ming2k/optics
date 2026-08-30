/* split_pane.c — interactive split divider pattern implementation. */

#include "../internal.h"
#include "../../include/lens/patterns.h"

bool lens_split_handle_v(lens *ui,
                         const char *id_str,
                         float *split_offset,
                         const lens_split_opts *opts) {
    if (!ui || !split_offset)
        return false;

    float min_s = (opts && opts->min_size > 0.0f) ? opts->min_size : 120.0f;
    float max_s = (opts && opts->max_size > min_s) ? opts->max_size : 600.0f;
    float handle_w = (opts && opts->handle_width > 0.0f) ? opts->handle_width : 2.0f;

    lens_push_id(ui, id_str ? id_str : "split_v");

    lens_separator_opts sep_opts = {
        .box = {
            .id = "split_bar",
            .width = handle_w,
        },
        .axis = LENS_COLUMN,
        .thickness = handle_w,
    };

    lens_response resp = lens_separator(ui, &sep_opts);

    /* Check dragging: when mouse is held on divider */
    bool changed = false;
    if (resp.pressed) {
        float new_offset = ui->input.cursor.x;
        if (new_offset < min_s) new_offset = min_s;
        if (new_offset > max_s) new_offset = max_s;
        if (new_offset != *split_offset) {
            *split_offset = new_offset;
            changed = true;
        }
    }

    if (resp.hovered || resp.pressed) {
        ui->cursor_hint = LENS_CURSOR_RESIZE_EW;
    }

    lens_pop_id(ui);
    return changed;
}
