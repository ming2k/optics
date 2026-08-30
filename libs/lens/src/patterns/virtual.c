/* virtual.c — virtual grid and list math calculations. */

#include "../internal.h"
#include "../../include/lens/patterns.h"

#include <math.h>

lens_virtual_grid_plan lens_virtual_grid_calc(float available_width,
                                              float viewport_height,
                                              float scroll_y,
                                              uint32_t total_items,
                                              float min_col_width,
                                              float max_col_width,
                                              float item_height,
                                              float target_gap,
                                              uint32_t overscan_rows) {
    lens_virtual_grid_plan plan = {0};

    if (available_width <= 0.0f || total_items == 0) {
        plan.columns = 1;
        return plan;
    }

    if (min_col_width <= 1.0f) min_col_width = 120.0f;
    if (max_col_width < min_col_width) max_col_width = min_col_width * 1.5f;
    if (item_height <= 1.0f) item_height = 100.0f;
    if (target_gap < 0.0f) target_gap = 12.0f;

    /* Calculate how many columns fit with target_gap */
    float unit = min_col_width + target_gap;
    uint32_t cols = (uint32_t)floorf((available_width + target_gap) / unit);
    if (cols < 1) cols = 1;

    /* Calculate allocated item width so columns span neatly */
    float total_gap_w = (cols > 1) ? (float)(cols - 1) * target_gap : 0.0f;
    float allocated_col_w = (available_width - total_gap_w) / (float)cols;
    if (allocated_col_w > max_col_width && cols > 1) {
        allocated_col_w = max_col_width;
    }

    uint32_t total_rows = (total_items + cols - 1) / cols;
    float row_pitch = item_height + target_gap;

    /* Viewport slice calculation */
    float safe_scroll_y = (scroll_y >= 0.0f) ? scroll_y : 0.0f;
    int32_t raw_first_row = (int32_t)floorf(safe_scroll_y / row_pitch);
    int32_t visible_count = (int32_t)ceilf(viewport_height / row_pitch) + 1;

    int32_t start_row = raw_first_row - (int32_t)overscan_rows;
    if (start_row < 0) start_row = 0;

    int32_t end_row = raw_first_row + visible_count + (int32_t)overscan_rows;
    if (end_row > (int32_t)total_rows) end_row = (int32_t)total_rows;

    plan.columns = cols;
    plan.column_width = allocated_col_w;
    plan.column_gap = target_gap;
    plan.row_pitch = row_pitch;
    plan.total_rows = total_rows;
    plan.visible_row_start = (uint32_t)start_row;
    plan.visible_row_end = (uint32_t)end_row;
    plan.top_padding = (float)start_row * row_pitch;
    plan.bottom_padding = ((float)total_rows - (float)end_row) * row_pitch;
    if (plan.bottom_padding < 0.0f) plan.bottom_padding = 0.0f;

    return plan;
}
