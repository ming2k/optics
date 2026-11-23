/* Public descriptors must produce observable behaviour, not just avoid crashes. */
#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {500, 300}, .dt_seconds = 0.016f, .cursor = {5, 5}};
    for (int frame = 0; frame < 2; frame++) {
        optics_ui_begin(ui, &in);
        optics_ui_row_begin(ui, &(optics_ui_layout_opts){});
        optics_ui_response r = optics_ui_button(
            ui, &(optics_ui_button_opts){
                    .label = "A", .box = {.min_width = 200, .min_height = 80, .tooltip = "hint"}});
        optics_ui_row_end(ui);
        optics_ui_test_end(ui);
        optics_gfx_rect rect = optics_ui_node_bounds(optics_ui_find(ui, r.id));
        OT_EXPECT_NEAR(rect.w, 200, 0.01);
        OT_EXPECT_NEAR(rect.h, 80, 0.01);
        if (frame) {
            OT_EXPECT(r.hovered);
            OT_EXPECT(ui->tooltip.active);
            OT_EXPECT(strcmp(ui->tooltip.text, "hint") == 0);
        }
    }
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.align = OPTICS_UI_END});
    optics_ui_response end =
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "end", .box = {.width = 50}});
    optics_ui_row_end(ui);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){});
    optics_ui_space_between(ui);
    optics_ui_response a =
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "left", .box = {.width = 50}});
    optics_ui_response b =
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "right", .box = {.width = 50}});
    optics_ui_row_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT_NEAR(optics_ui_node_bounds(optics_ui_find(ui, end.id)).x, 450, 0.01);
    OT_EXPECT_NEAR(optics_ui_node_bounds(optics_ui_find(ui, a.id)).x, 0, 0.01);
    OT_EXPECT_NEAR(optics_ui_node_bounds(optics_ui_find(ui, b.id)).x, 450, 0.01);

    optics_ui_begin(ui, &in);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){});
    optics_ui_fit(ui);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "fit", .box = {.width = 60}});
    optics_ui_column_end(ui);
    optics_ui_grid_begin(ui, &(optics_ui_grid_opts){
                                 .box = {.width = 200}, .columns = 2, .col_gap = 10, .row_gap = 7});
    optics_ui_response cells[3];
    for (int i = 0; i < 3; i++) {
        char id[8];
        snprintf(id, sizeof id, "cell%d", i);
        cells[i] =
            optics_ui_button(ui, &(optics_ui_button_opts){.label = id, .box = {.height = 20}});
    }
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT_NEAR(optics_ui_node_bounds(optics_ui_node_first_child(optics_ui_root(ui))).w, 60,
                   0.01);
    optics_gfx_rect c0 = optics_ui_node_bounds(optics_ui_find(ui, cells[0].id));
    optics_gfx_rect c1 = optics_ui_node_bounds(optics_ui_find(ui, cells[1].id));
    optics_gfx_rect c2 = optics_ui_node_bounds(optics_ui_find(ui, cells[2].id));
    OT_EXPECT_NEAR(c0.w, 95, 0.01);
    OT_EXPECT_NEAR(c1.x - c0.x, 105, 0.01);
    OT_EXPECT_NEAR(c1.y, c0.y, 0.01);
    OT_EXPECT_NEAR(c2.x, c0.x, 0.01);
    OT_EXPECT_NEAR(c2.y - c0.y, 27, 0.01);

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box = {.disabled = true}});
    optics_ui_response disabled = optics_ui_button(ui, &(optics_ui_button_opts){.label = "child"});
    optics_ui_row_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT((disabled.state & OPTICS_UI_STATE_DISABLED) != 0);

    /* ADR-0094: Full Scene Snapshot Compilation into DisplayList without GPU */
    optics_ui_begin(ui, &in);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.width = 300, .height = 200}});
    optics_ui_button(ui,
                     &(optics_ui_button_opts){.label = "OK", .box = {.width = 100, .height = 40}});
    optics_ui_label(
        ui, &(optics_ui_label_opts){.text = "Snapshot Test", .box = {.width = 150, .height = 25}});
    optics_ui_column_end(ui);
    optics_ui_test_end(ui);

    optics_ui_scene_snapshot *first_snapshot = nullptr;
    OT_EXPECT(optics_ui_snapshot_create(ui, &first_snapshot) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_display_list_command_count(
                  optics_ui_snapshot_display_list(first_snapshot)) >= 3);

    /* Submit to CPU canvas and verify replay */
    optics_gfx_canvas_desc cd = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 500, .height = 300);
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &c) == OPTICS_GFX_OK);
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(first_snapshot, c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t pw = 0, ph = 0, pstride = 0;
    const uint8_t *px = optics_gfx_canvas_read_pixels(c, &pw, &ph, &pstride);
    OT_EXPECT(px != nullptr);

    optics_ui_snapshot_release(first_snapshot);
    optics_gfx_canvas_release(c);

    /* ADR-0094: ui Scene Snapshot Independence & Survives UI Context Destruction */
    optics_ui_scene_snapshot *snapshot = nullptr;
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    OT_EXPECT(snapshot != nullptr);
    uint64_t gen = optics_ui_snapshot_generation(snapshot);
    OT_EXPECT(gen == optics_ui_generation(ui));
    OT_EXPECT(optics_ui_last_presented_generation(ui) == optics_ui_generation(ui));

    /* Retain snapshot */
    optics_ui_scene_snapshot *retained_snap = optics_ui_snapshot_retain(snapshot);
    OT_EXPECT(retained_snap == snapshot);
    optics_ui_snapshot_release(retained_snap);

    /* Destroy mutable UI context and all temporary arena inputs */
    optics_ui_release(ui);

    /* Replay published snapshot on a fresh canvas — must succeed even after UI is destroyed */
    OT_EXPECT(optics_gfx_canvas_create(&cd, &c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(snapshot, c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    px = optics_gfx_canvas_read_pixels(c, &pw, &ph, &pstride);
    OT_EXPECT(px != nullptr);

    optics_ui_snapshot_release(snapshot);
    optics_gfx_canvas_release(c);

    /* Presentation Handshake & Generation Progression (ADR-0094) */
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    uint64_t g0 = optics_ui_generation(ui);
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Frame 1"});
    optics_ui_end(ui);
    uint64_t g1 = optics_ui_generation(ui);
    OT_EXPECT(g1 > g0);
    OT_EXPECT(optics_ui_last_presented_generation(ui) == 0);

    /* Simulate successful presentation of Frame 1 */
    optics_ui_scene_snapshot *presented = nullptr;
    OT_EXPECT(optics_ui_snapshot_create(ui, &presented) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_activate(ui, presented) == OPTICS_GFX_OK);
    optics_ui_snapshot_release(presented);
    OT_EXPECT(optics_ui_last_presented_generation(ui) == g1);

    /* Frame 2: simulated presentation failure (drop without notify) */
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Frame 2"});
    optics_ui_end(ui);
    uint64_t g2 = optics_ui_generation(ui);
    OT_EXPECT(g2 > g1);
    /* Failed presentation: last presented generation MUST NOT advance! */
    OT_EXPECT(optics_ui_last_presented_generation(ui) == g1);

    optics_ui_release(ui);
    optics_ui_fit(nullptr);
    optics_ui_space_between(nullptr);
    return OT_REPORT();
}
