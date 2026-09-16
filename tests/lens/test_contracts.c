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
        test_end(ui);
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
    test_end(ui);
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
    test_end(ui);
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
    test_end(ui);
    CHECK((disabled.state & LENS_STATE_DISABLED) != 0);

    /* ADR-0094: Full Scene Snapshot Compilation into DisplayList without GPU */
    lens_begin(ui, &in);
    lens_column_begin(ui, &(lens_layout_opts){.box = {.width = 300, .height = 200}});
    lens_button(ui, &(lens_button_opts){.label = "OK", .box = {.width = 100, .height = 40}});
    lens_label(ui,
               &(lens_label_opts){.text = "Snapshot Test", .box = {.width = 150, .height = 25}});
    lens_column_end(ui);
    test_end(ui);

    lens_scene_snapshot *first_snapshot = nullptr;
    CHECK(lens_snapshot_create(ui, &first_snapshot) == FLUX_OK);
    CHECK(flux_display_list_command_count(lens_snapshot_display_list(first_snapshot)) >= 3);

    /* Submit to CPU canvas and verify replay */
    flux_canvas_desc cd =
        FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU, .width = 500, .height = 300);
    flux_canvas *c = nullptr;
    CHECK(flux_canvas_create(&cd, &c) == FLUX_OK);
    flux_color clear = 0;
    CHECK(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &clear}) == FLUX_OK);
    CHECK(lens_snapshot_submit(first_snapshot, c) == FLUX_OK);
    CHECK(flux_canvas_end(c) == FLUX_OK);

    uint32_t pw = 0, ph = 0, pstride = 0;
    const uint8_t *px = flux_canvas_read_pixels(c, &pw, &ph, &pstride);
    CHECK(px != nullptr);

    lens_snapshot_release(first_snapshot);
    flux_canvas_release(c);

    /* ADR-0094: Lens Scene Snapshot Independence & Survives UI Context Destruction */
    lens_scene_snapshot *snapshot = nullptr;
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    CHECK(snapshot != nullptr);
    uint64_t gen = lens_snapshot_generation(snapshot);
    CHECK(gen == lens_generation(ui));
    CHECK(lens_last_presented_generation(ui) == lens_generation(ui));

    /* Retain snapshot */
    lens_scene_snapshot *retained_snap = lens_snapshot_retain(snapshot);
    CHECK(retained_snap == snapshot);
    lens_snapshot_release(retained_snap);

    /* Destroy mutable UI context and all temporary arena inputs */
    lens_release(ui);

    /* Replay published snapshot on a fresh canvas — must succeed even after UI is destroyed */
    CHECK(flux_canvas_create(&cd, &c) == FLUX_OK);
    CHECK(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                        .clear_color = &clear}) == FLUX_OK);
    CHECK(lens_snapshot_submit(snapshot, c) == FLUX_OK);
    CHECK(flux_canvas_end(c) == FLUX_OK);
    px = flux_canvas_read_pixels(c, &pw, &ph, &pstride);
    CHECK(px != nullptr);

    lens_snapshot_release(snapshot);
    flux_canvas_release(c);

    /* Presentation Handshake & Generation Progression (ADR-0094) */
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    uint64_t g0 = lens_generation(ui);
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "Frame 1"});
    lens_end(ui);
    uint64_t g1 = lens_generation(ui);
    CHECK(g1 > g0);
    CHECK(lens_last_presented_generation(ui) == 0);

    /* Simulate successful presentation of Frame 1 */
    lens_scene_snapshot *presented = nullptr;
    CHECK(lens_snapshot_create(ui, &presented) == FLUX_OK);
    CHECK(lens_snapshot_activate(ui, presented) == FLUX_OK);
    lens_snapshot_release(presented);
    CHECK(lens_last_presented_generation(ui) == g1);

    /* Frame 2: simulated presentation failure (drop without notify) */
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "Frame 2"});
    lens_end(ui);
    uint64_t g2 = lens_generation(ui);
    CHECK(g2 > g1);
    /* Failed presentation: last presented generation MUST NOT advance! */
    CHECK(lens_last_presented_generation(ui) == g1);

    lens_release(ui);
    lens_fit(nullptr);
    lens_space_between(nullptr);
    return TEST_REPORT();
}
