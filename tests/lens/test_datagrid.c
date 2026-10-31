/* test_datagrid.c — verification for high-performance interactive data grid pattern. */

#include "test_helpers.h"
#include <lens/patterns.h>

int main(void) {
    lens *ui = nullptr;
    CHECK(lens_create(nullptr, &ui) == FLUX_OK);
    CHECK(ui != nullptr);

    lens_datagrid_col cols[3] = {
        {.title = "ID", .width = 80.0f, .sortable = true, .resizable = true},
        {.title = "Name", .width = 150.0f, .sortable = true, .resizable = true},
        {.title = "Role", .width = 120.0f, .sortable = false, .resizable = false},
    };

    const char *cells[12] = {
        "1", "Alice",   "Admin",    "2", "Bob",   "Developer",
        "3", "Charlie", "Designer", "4", "David", "QA",
    };

    uint32_t selected = 0;
    lens_datagrid_opts opts = {
        .header_height = 32.0f,
        .row_height = 28.0f,
        .striped = true,
    };

    lens_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: Neutral layout pass */
    lens_begin(ui, &in);
    lens_datagrid_action a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    CHECK(a.kind == LENS_DATAGRID_ACTION_NONE);
    test_end(ui);

    /* Frame 2: Click on column header 1 ("Name") to trigger sort */
    in.cursor = (flux_point){120.0f, 16.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    test_end(ui);

    /* Release click on column 1 */
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    CHECK(a.kind == LENS_DATAGRID_ACTION_SORT_CHANGED);
    CHECK(a.col_index == 1);
    CHECK(a.sort_dir == LENS_SORT_ASC);
    test_end(ui);

    /* Click again to toggle sort to DESC */
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    CHECK(a.kind == LENS_DATAGRID_ACTION_SORT_CHANGED);
    CHECK(a.col_index == 1);
    CHECK(a.sort_dir == LENS_SORT_DESC);
    test_end(ui);

    /* Frame 3: Click on row 2 */
    in.cursor = (flux_point){50.0f, 32.0f + 28.0f * 2.0f + 10.0f};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    test_end(ui);

    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    a = lens_datagrid(ui, "my_grid", cols, 3, cells, 4, &selected, &opts);
    CHECK(a.kind == LENS_DATAGRID_ACTION_ROW_CLICKED);
    CHECK(a.row_index == 2);
    CHECK(selected == 2);
    test_end(ui);

    lens_release(ui);
    return TEST_REPORT();
}
