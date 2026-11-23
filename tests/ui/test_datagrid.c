/* test_datagrid.c — verification for high-performance interactive data grid pattern. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui/patterns.h>

/* The fixed arguments of every case in this file, merged into a copy of the
 * case's options. */
static optics_ui_datagrid_action datagrid(optics_ui *ui, const char *id,
                                          optics_ui_datagrid_col *cols, uint32_t col_count,
                                          const char *const *cell_texts, uint32_t row_count,
                                          uint32_t *selected_row,
                                          const optics_ui_datagrid_opts *base) {
    optics_ui_datagrid_opts o = *base;
    o.id = id;
    o.cols = cols;
    o.col_count = col_count;
    o.cell_texts = cell_texts;
    o.row_count = row_count;
    o.selected_row = selected_row;
    return optics_ui_datagrid(ui, &o);
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(nullptr, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(ui != nullptr);

    optics_ui_datagrid_col cols[3] = {
        {.title = "ID", .width = 80.0f, .sortable = true, .resizable = true},
        {.title = "Name", .width = 150.0f, .sortable = true, .resizable = true},
        {.title = "Role", .width = 120.0f, .sortable = false, .resizable = false},
    };

    const char *cells[12] = {
        "1", "Alice",   "Admin",    "2", "Bob",   "Developer",
        "3", "Charlie", "Designer", "4", "David", "QA",
    };

    uint32_t selected = 0;
    optics_ui_datagrid_opts opts = {
        .header_height = 32.0f,
        .row_height = 28.0f,
        .striped = true,
    };

    optics_ui_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: Neutral layout pass */
    optics_ui_begin(ui, &in);
    optics_ui_datagrid_action a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_DATAGRID_ACTION_NONE);
    optics_ui_test_end(ui);

    /* Frame 2: Click on column header 1 ("Name") to trigger sort */
    in.cursor = (optics_gfx_point){120.0f, 16.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    optics_ui_test_end(ui);

    /* Release click on column 1 */
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_DATAGRID_ACTION_SORT_CHANGED);
    OT_EXPECT(a.col_index == 1);
    OT_EXPECT(a.sort_dir == OPTICS_UI_SORT_ASC);
    optics_ui_test_end(ui);

    /* Click again to toggle sort to DESC */
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_DATAGRID_ACTION_SORT_CHANGED);
    OT_EXPECT(a.col_index == 1);
    OT_EXPECT(a.sort_dir == OPTICS_UI_SORT_DESC);
    optics_ui_test_end(ui);

    /* Frame 3: Click on row 2 */
    in.cursor = (optics_gfx_point){50.0f, 32.0f + 28.0f * 2.0f + 10.0f};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    optics_ui_test_end(ui);

    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    a = datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    OT_EXPECT(a.kind == OPTICS_UI_DATAGRID_ACTION_ROW_CLICKED);
    OT_EXPECT(a.row_index == 2);
    OT_EXPECT(selected == 2);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
    return OT_REPORT();
}
