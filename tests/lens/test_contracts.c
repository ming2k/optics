/* Public descriptors must produce observable behaviour, not just avoid crashes. */
#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"

int main(void) {
    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {500, 300}, .dt_seconds = 0.016f, .cursor = {5, 5}};
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &in);
        lens_row_begin(ui, &(lens_layout_opts){});
        lens_response r = lens_button(
            ui, &(lens_button_opts){
                    .label = "A", .box = {.min_width = 200, .min_height = 80, .tooltip = "hint"}});
        lens_row_end(ui);
        lens_end(ui);
        flux_rect rect = lens_node_bounds(lens_find(ui, r.id));
        CHECK_NEAR(rect.w, 200, 0.01);
        CHECK_NEAR(rect.h, 80, 0.01);
        if (frame) {
            CHECK(r.hovered);
            CHECK(ui->tooltip.active);
            CHECK(strcmp(ui->tooltip.text, "hint") == 0);
        }
    }
    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.align = LENS_END});
    lens_response end = lens_button(ui, &(lens_button_opts){.label = "end", .box = {.width = 50}});
    lens_row_end(ui);
    lens_row_begin(ui, &(lens_layout_opts){});
    lens_space_between(ui);
    lens_response a = lens_button(ui, &(lens_button_opts){.label = "left", .box = {.width = 50}});
    lens_response b = lens_button(ui, &(lens_button_opts){.label = "right", .box = {.width = 50}});
    lens_row_end(ui);
    lens_end(ui);
    CHECK_NEAR(lens_node_bounds(lens_find(ui, end.id)).x, 450, 0.01);
    CHECK_NEAR(lens_node_bounds(lens_find(ui, a.id)).x, 0, 0.01);
    CHECK_NEAR(lens_node_bounds(lens_find(ui, b.id)).x, 450, 0.01);

    lens_begin(ui, &in);
    lens_column_begin(ui, &(lens_layout_opts){});
    lens_fit(ui);
    lens_label(ui, &(lens_label_opts){.text = "fit", .box = {.width = 60}});
    lens_column_end(ui);
    lens_grid_begin(
        ui, &(lens_grid_opts){.box = {.width = 200}, .columns = 2, .col_gap = 10, .row_gap = 7});
    lens_response cells[3];
    for (int i = 0; i < 3; i++) {
        char id[8];
        snprintf(id, sizeof id, "cell%d", i);
        cells[i] = lens_button(ui, &(lens_button_opts){.label = id, .box = {.height = 20}});
    }
    lens_grid_end(ui);
    lens_end(ui);
    CHECK_NEAR(lens_node_bounds(lens_node_first_child(lens_root(ui))).w, 60, 0.01);
    flux_rect c0 = lens_node_bounds(lens_find(ui, cells[0].id));
    flux_rect c1 = lens_node_bounds(lens_find(ui, cells[1].id));
    flux_rect c2 = lens_node_bounds(lens_find(ui, cells[2].id));
    CHECK_NEAR(c0.w, 95, 0.01);
    CHECK_NEAR(c1.x - c0.x, 105, 0.01);
    CHECK_NEAR(c1.y, c0.y, 0.01);
    CHECK_NEAR(c2.x, c0.x, 0.01);
    CHECK_NEAR(c2.y - c0.y, 27, 0.01);

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.box = {.disabled = true}});
    lens_response disabled = lens_button(ui, &(lens_button_opts){.label = "child"});
    lens_row_end(ui);
    lens_end(ui);
    CHECK((disabled.state & LENS_STATE_DISABLED) != 0);

    /* ADR-0094: Full Scene Snapshot Compilation into DisplayList without GPU */
    lens_begin(ui, &in);
    lens_column_begin(ui, &(lens_layout_opts){.box = {.width = 300, .height = 200}});
    lens_button(ui, &(lens_button_opts){.label = "OK", .box = {.width = 100, .height = 40}});
    lens_label(ui, &(lens_label_opts){.text = "Snapshot Test", .box = {.width = 150, .height = 25}});
    lens_column_end(ui);
    lens_end(ui);

    flux_arena snapshot_arena;
    CHECK(flux_arena_init(&snapshot_arena, 65536, nullptr) == FLUX_OK);
    lens_draw_list draw_list = {0};
    CHECK(lens_compile_draw_list(ui, &snapshot_arena, &draw_list) == FLUX_OK);
    /* Verify that compile_draw_list captured the complete UI tree into commands (NOT just 1 dummy command!) */
    CHECK(flux_display_list_command_count(draw_list.display_list) >= 3);
    CHECK(flux_display_list_size(draw_list.display_list) > 0);

    /* Submit to CPU canvas and verify replay */
    flux_canvas_desc cd = FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU, .width = 500, .height = 300);
    flux_canvas *c = nullptr;
    CHECK(flux_canvas_create(&cd, &c) == FLUX_OK);
    flux_color clear = 0;
    CHECK(flux_canvas_begin(c, nullptr, &clear) == FLUX_OK);
    CHECK(lens_draw_list_submit(&draw_list, c) == FLUX_OK);
    CHECK(flux_canvas_end(c) == FLUX_OK);

    uint32_t pw = 0, ph = 0, pstride = 0;
    const uint8_t *px = flux_canvas_read_pixels(c, &pw, &ph, &pstride);
    CHECK(px != nullptr);

    flux_display_list_release(draw_list.display_list);
    flux_canvas_release(c);
    flux_arena_deinit(&snapshot_arena);

    lens_release(ui);
    lens_fit(nullptr);
    lens_space_between(nullptr);
    return TEST_REPORT();
}
