/* datagrid.c — high-performance interactive data grid pattern (ADR-Pattern-005). */

#include "../internal.h"
#include <optics/ui/patterns.h>

#include <math.h>
#include <stdio.h>

#define DATAGRID_SPLIT_HIT_W 6.0f

typedef struct datagrid_drag {
    int32_t resize_col;
    float start_x;
    float start_w;
} datagrid_drag;

optics_ui_datagrid_action optics_ui_datagrid(optics_ui *ui, const optics_ui_datagrid_opts *opts) {
    if (!opts)
        return (optics_ui_datagrid_action){};
    const char *id_str = opts->id;
    optics_ui_datagrid_col *cols = opts->cols;
    const uint32_t col_count = opts->col_count;
    const char *const *cell_texts = opts->cell_texts;
    const uint32_t row_count = opts->row_count;
    uint32_t *selected_row = opts->selected_row;

    optics_ui_datagrid_action action = {
        .kind = OPTICS_UI_DATAGRID_ACTION_NONE,
        .row_index = 0,
        .col_index = 0,
        .sort_dir = OPTICS_UI_SORT_NONE,
    };

    if (!ui || !cols || col_count == 0)
        return action;

    optics_ui_push_id(ui, id_str ? id_str : "datagrid");

    float header_h = (opts && opts->header_height > 0.0f) ? opts->header_height : 32.0f;
    float row_h = (opts && opts->row_height > 0.0f) ? opts->row_height : 28.0f;
    bool striped = opts ? opts->striped : false;

    /* Normalize column widths */
    float total_table_w = 0.0f;
    for (uint32_t i = 0; i < col_count; i++) {
        float min_w = cols[i].min_width > 0.0f ? cols[i].min_width : 40.0f;
        if (cols[i].width <= 0.0f)
            cols[i].width = 120.0f;
        if (cols[i].width < min_w)
            cols[i].width = min_w;
        total_table_w += cols[i].width;
    }

    /* Column resize drag state */
    optics_ui_id drag_id = optics_ui_current_id(ui, "datagrid_drag");
    optics_ui_node *drag_node = optics_ui__store_touch(ui, drag_id);
    datagrid_drag *drag =
        drag_node ? (datagrid_drag *)optics_ui_node_state(drag_node, sizeof(datagrid_drag))
                  : nullptr;
    if (drag && !ui->input.mouse_down[OPTICS_UI_MOUSE_LEFT]) {
        drag->resize_col = -1;
    }

    /* Outer column container */
    optics_ui_column_begin(
        ui, &(optics_ui_layout_opts){
                .box = {.width = total_table_w, .height = opts ? opts->height : 0.0f},
                .gap = 0.0f,
                .pad = 0.0f,
            });

    float scroll_x = 0.0f, scroll_y = 0.0f;
    (void)optics_ui_scroll_offset(ui, &(optics_ui_scroll_offset_opts){.id = "grid_scroll",
                                                                      .out_x = &scroll_x,
                                                                      .out_y = &scroll_y});

    /* -------------------------------------------------------------- */
    /*  1. Header Row (Sticky in Y, synchronized in X)                */
    /* -------------------------------------------------------------- */
    optics_ui_scroll_begin(
        ui, &(optics_ui_scroll_opts){
                .box = {.id = "grid_hdr_scroll", .width = total_table_w, .height = header_h},
                .autohide = true,
            });
    optics_ui_scroll_to(
        ui, &(optics_ui_scroll_to_opts){.id = "grid_hdr_scroll", .x = scroll_x, .y = 0.0f});

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .box = {.width = total_table_w, .height = header_h},
                                .bg = ui->theme.color_surface_sunken,
                                .border = ui->theme.color_border,
                                .border_width = 1.0f,
                                .cross = OPTICS_UI_CENTER,
                            });

    for (uint32_t i = 0; i < col_count; i++) {
        optics_ui_datagrid_col *c = &cols[i];
        char col_id[32];
        snprintf(col_id, sizeof(col_id), "hdr_%u", i);

        /* Handle column header button / sort trigger */
        optics_ui_response r = optics_ui_button(
            ui,
            &(optics_ui_button_opts){
                .box = {.id = col_id, .width = c->width - DATAGRID_SPLIT_HIT_W, .height = header_h},
                .label = c->title,
                .variant = OPTICS_UI_BUTTON_SUBTLE,
                .icon = (c->sort == OPTICS_UI_SORT_ASC)    ? OPTICS_UI_ICON_CHEVRON_UP
                        : (c->sort == OPTICS_UI_SORT_DESC) ? OPTICS_UI_ICON_CHEVRON_DOWN
                                                           : 0,
            });

        if (r.clicked && c->sortable) {
            c->sort = (c->sort == OPTICS_UI_SORT_ASC) ? OPTICS_UI_SORT_DESC : OPTICS_UI_SORT_ASC;
            action.kind = OPTICS_UI_DATAGRID_ACTION_SORT_CHANGED;
            action.col_index = i;
            action.sort_dir = c->sort;
        }

        /* Resizing divider handle */
        if (c->resizable) {
            char sep_id[32];
            snprintf(sep_id, sizeof(sep_id), "sep_%u", i);
            optics_ui_response sep_r = optics_ui_separator(
                ui, &(optics_ui_separator_opts){
                        .box = {.id = sep_id, .width = DATAGRID_SPLIT_HIT_W, .height = header_h},
                        .axis = OPTICS_UI_COLUMN,
                        .thickness = 1.0f,
                    });

            if (sep_r.hovered || (drag && drag->resize_col == (int32_t)i)) {
                ui->cursor_hint = OPTICS_UI_CURSOR_RESIZE_EW;
            }

            if (sep_r.pressed && drag && drag->resize_col < 0) {
                drag->resize_col = (int32_t)i;
                drag->start_x = ui->input.cursor.x;
                drag->start_w = c->width;
            }

            if (drag && drag->resize_col == (int32_t)i &&
                ui->input.mouse_down[OPTICS_UI_MOUSE_LEFT]) {
                float dx = ui->input.cursor.x - drag->start_x;
                float min_w = c->min_width > 0.0f ? c->min_width : 40.0f;
                float new_w = fmaxf(min_w, drag->start_w + dx);
                if (new_w != c->width) {
                    c->width = new_w;
                    action.kind = OPTICS_UI_DATAGRID_ACTION_COL_RESIZED;
                    action.col_index = i;
                }
            }
        }
    }
    optics_ui_row_end(ui);
    optics_ui_scroll_end(ui);

    /* -------------------------------------------------------------- */
    /*  2. Virtualized Row Body                                       */
    /* -------------------------------------------------------------- */
    float body_h = (opts && opts->height > header_h) ? (opts->height - header_h) : 240.0f;
    optics_ui_scroll_begin(
        ui, &(optics_ui_scroll_opts){
                .box = {.id = "grid_scroll", .width = total_table_w, .height = body_h},
                .autohide = true,
            });

    optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                   .box = {.width = total_table_w},
                                   .gap = 0.0f,
                                   .pad = 0.0f,
                               });

    uint32_t start_row = (uint32_t)floorf(fmaxf(0.0f, scroll_y) / row_h);
    uint32_t visible_count = (uint32_t)ceilf(body_h / row_h) + 2;
    uint32_t end_row = start_row + visible_count;
    if (end_row > row_count)
        end_row = row_count;

    float top_pad = start_row * row_h;
    float bottom_pad = (row_count > end_row) ? (float)(row_count - end_row) * row_h : 0.0f;

    if (top_pad > 0.0f) {
        optics_ui_spacer(ui, top_pad);
    }

    for (uint32_t r = start_row; r < end_row; r++) {
        bool is_selected = (selected_row && *selected_row == r);
        char row_id[32];
        snprintf(row_id, sizeof(row_id), "row_%u", r);

        optics_gfx_color row_bg = is_selected                 ? ui->theme.color_active
                                  : (striped && (r % 2 == 1)) ? ui->theme.color_surface_sunken
                                                              : (optics_gfx_color){};

        optics_ui_response row_resp = optics_ui_pressable_begin(
            ui, &(optics_ui_pressable_opts){
                    .box = {.id = row_id, .width = total_table_w, .height = row_h},
                    .label = row_id,
                    .layout =
                        {
                            .bg = row_bg,
                            .cross = OPTICS_UI_CENTER,
                        },
                });

        if (row_resp.clicked) {
            if (selected_row)
                *selected_row = r;
            action.kind = OPTICS_UI_DATAGRID_ACTION_ROW_CLICKED;
            action.row_index = r;
        }

        /* Render cells in row */
        for (uint32_t c = 0; c < col_count; c++) {
            const char *text = (cell_texts) ? cell_texts[r * col_count + c] : "";
            optics_ui_label(ui, &(optics_ui_label_opts){
                                    .box = {.width = cols[c].width, .height = row_h},
                                    .text = text ? text : "",
                                    .align = cols[c].align,
                                });
        }

        optics_ui_pressable_end(ui);
    }

    if (bottom_pad > 0.0f) {
        optics_ui_spacer(ui, bottom_pad);
    }

    optics_ui_column_end(ui);
    optics_ui_scroll_end(ui);
    optics_ui_column_end(ui);
    optics_ui_pop_id(ui);

    return action;
}
