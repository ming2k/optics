/* datagrid.c — high-performance interactive data grid pattern (ADR-Pattern-005). */

#include "../../include/lens/patterns.h"
#include "../internal.h"

#include <math.h>
#include <stdio.h>

#define DATAGRID_SPLIT_HIT_W 6.0f

typedef struct datagrid_drag {
    int32_t resize_col;
    float start_x;
    float start_w;
} datagrid_drag;

lens_datagrid_action lens_datagrid(lens *ui, const char *id_str, lens_datagrid_col *cols,
                                   uint32_t col_count, const char *const *cell_texts,
                                   uint32_t row_count, uint32_t *selected_row,
                                   const lens_datagrid_opts *opts) {
    lens_datagrid_action action = {
        .kind = LENS_DATAGRID_ACTION_NONE,
        .row_index = 0,
        .col_index = 0,
        .sort_dir = LENS_SORT_NONE,
    };

    if (!ui || !cols || col_count == 0)
        return action;

    lens_push_id(ui, id_str ? id_str : "datagrid");

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
    lens_id drag_id = lens_current_id(ui, "datagrid_drag");
    lens_node *drag_node = lensi_store_touch(ui, drag_id);
    datagrid_drag *drag =
        drag_node ? (datagrid_drag *)lens_node_state(drag_node, sizeof(datagrid_drag)) : nullptr;
    if (drag && !ui->input.mouse_down[LENS_MOUSE_LEFT]) {
        drag->resize_col = -1;
    }

    /* Outer column container */
    lens_column_begin(ui, &(lens_layout_opts){
                              .box = {.width = total_table_w, .height = opts ? opts->height : 0.0f},
                              .gap = 0.0f,
                              .pad = 0.0f,
                          });

    float scroll_x = 0.0f, scroll_y = 0.0f;
    (void)lens_scroll_offset(ui, "grid_scroll", &scroll_x, &scroll_y);

    /* -------------------------------------------------------------- */
    /*  1. Header Row (Sticky in Y, synchronized in X)                */
    /* -------------------------------------------------------------- */
    lens_scroll_begin(
        ui, &(lens_scroll_opts){
                .box = {.id = "grid_hdr_scroll", .width = total_table_w, .height = header_h},
                .autohide = true,
            });
    lens_scroll_to(ui, "grid_hdr_scroll", scroll_x, 0.0f);

    lens_row_begin(ui, &(lens_layout_opts){
                           .box = {.width = total_table_w, .height = header_h},
                           .bg = ui->theme.color_surface_sunken,
                           .border = ui->theme.color_border,
                           .border_width = 1.0f,
                           .cross = LENS_CENTER,
                       });

    for (uint32_t i = 0; i < col_count; i++) {
        lens_datagrid_col *c = &cols[i];
        char col_id[32];
        snprintf(col_id, sizeof(col_id), "hdr_%u", i);

        /* Handle column header button / sort trigger */
        lens_response r = lens_button(
            ui,
            &(lens_button_opts){
                .box = {.id = col_id, .width = c->width - DATAGRID_SPLIT_HIT_W, .height = header_h},
                .label = c->title,
                .variant = LENS_BUTTON_SUBTLE,
                .icon = (c->sort == LENS_SORT_ASC)    ? LENS_ICON_CHEVRON_UP
                        : (c->sort == LENS_SORT_DESC) ? LENS_ICON_CHEVRON_DOWN
                                                      : 0,
            });

        if (r.clicked && c->sortable) {
            c->sort = (c->sort == LENS_SORT_ASC) ? LENS_SORT_DESC : LENS_SORT_ASC;
            action.kind = LENS_DATAGRID_ACTION_SORT_CHANGED;
            action.col_index = i;
            action.sort_dir = c->sort;
        }

        /* Resizing divider handle */
        if (c->resizable) {
            char sep_id[32];
            snprintf(sep_id, sizeof(sep_id), "sep_%u", i);
            lens_response sep_r = lens_separator(
                ui, &(lens_separator_opts){
                        .box = {.id = sep_id, .width = DATAGRID_SPLIT_HIT_W, .height = header_h},
                        .axis = LENS_COLUMN,
                        .thickness = 1.0f,
                    });

            if (sep_r.hovered || (drag && drag->resize_col == (int32_t)i)) {
                ui->cursor_hint = LENS_CURSOR_RESIZE_EW;
            }

            if (sep_r.pressed && drag && drag->resize_col < 0) {
                drag->resize_col = (int32_t)i;
                drag->start_x = ui->input.cursor.x;
                drag->start_w = c->width;
            }

            if (drag && drag->resize_col == (int32_t)i && ui->input.mouse_down[LENS_MOUSE_LEFT]) {
                float dx = ui->input.cursor.x - drag->start_x;
                float min_w = c->min_width > 0.0f ? c->min_width : 40.0f;
                float new_w = fmaxf(min_w, drag->start_w + dx);
                if (new_w != c->width) {
                    c->width = new_w;
                    action.kind = LENS_DATAGRID_ACTION_COL_RESIZED;
                    action.col_index = i;
                }
            }
        }
    }
    lens_row_end(ui);
    lens_scroll_end(ui);

    /* -------------------------------------------------------------- */
    /*  2. Virtualized Row Body                                       */
    /* -------------------------------------------------------------- */
    float body_h = (opts && opts->height > header_h) ? (opts->height - header_h) : 240.0f;
    lens_scroll_begin(ui,
                      &(lens_scroll_opts){
                          .box = {.id = "grid_scroll", .width = total_table_w, .height = body_h},
                          .autohide = true,
                      });

    lens_column_begin(ui, &(lens_layout_opts){
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
        lens_spacer(ui, top_pad);
    }

    for (uint32_t r = start_row; r < end_row; r++) {
        bool is_selected = (selected_row && *selected_row == r);
        char row_id[32];
        snprintf(row_id, sizeof(row_id), "row_%u", r);

        flux_color row_bg = is_selected                 ? ui->theme.color_active
                            : (striped && (r % 2 == 1)) ? ui->theme.color_surface_sunken
                                                        : (flux_color){0};

        lens_response row_resp = lens_pressable_begin(
            ui, &(lens_pressable_opts){
                    .box = {.id = row_id, .width = total_table_w, .height = row_h},
                    .label = row_id,
                    .layout =
                        {
                            .bg = row_bg,
                            .cross = LENS_CENTER,
                        },
                });

        if (row_resp.clicked) {
            if (selected_row)
                *selected_row = r;
            action.kind = LENS_DATAGRID_ACTION_ROW_CLICKED;
            action.row_index = r;
        }

        /* Render cells in row */
        for (uint32_t c = 0; c < col_count; c++) {
            const char *text = (cell_texts) ? cell_texts[r * col_count + c] : "";
            lens_label(ui, &(lens_label_opts){
                               .box = {.width = cols[c].width, .height = row_h},
                               .text = text ? text : "",
                               .align = cols[c].align,
                           });
        }

        lens_pressable_end(ui);
    }

    if (bottom_pad > 0.0f) {
        lens_spacer(ui, bottom_pad);
    }

    lens_column_end(ui);
    lens_scroll_end(ui);
    lens_column_end(ui);
    lens_pop_id(ui);

    return action;
}
