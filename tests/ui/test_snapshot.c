/* Publication, presentation and failure semantics through the public snapshot
 * API. Internal access is used only to inject scratch-allocation failure. */
#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui/icon.h>
#include <stdlib.h>
#include <string.h>

static optics_ui_response build(optics_ui *ui, const optics_ui_input *in, float pad,
                                const char *label) {
    optics_ui_begin(ui, in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box = {.id = "panel"}, .pad = pad});
    optics_ui_response result = optics_ui_button(
        ui, &(optics_ui_button_opts){.label = label,
                                     .box = {.id = "action", .width = 80, .height = 30}});
    optics_ui_row_end(ui);
    optics_ui_end(ui); /* Deliberately do not activate the newly built geometry. */
    return result;
}

typedef struct {
    const char *expected;
    optics_gfx_rect bounds;
    unsigned buttons;
} semantic_check;

static void visit(const optics_ui_semantics *s, optics_gfx_rect bounds, optics_ui_id id,
                  optics_ui_id parent, void *user) {
    semantic_check *check = user;
    if (s->role != OPTICS_UI_ROLE_BUTTON)
        return;
    OT_EXPECT(s->name && strcmp(s->name, check->expected) == 0);
    check->bounds = bounds;
    check->buttons++;
}

static void delayed_presentation_and_owned_metadata(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {400, 240}, .dt_seconds = .016f};
    optics_ui_response response = build(ui, &in, 0, "old");
    optics_gfx_rect original = optics_ui_node_bounds(optics_ui_find(ui, response.id));
    in.cursor = (optics_gfx_point){original.x + 5, original.y + 5};
    optics_ui_scene_snapshot *a = nullptr, *b = nullptr;
    OT_EXPECT(optics_ui_snapshot_create(ui, &a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_last_presented_generation(ui) == 0);

    response = build(ui, &in, 60, "new");
    OT_EXPECT(!response.hovered); /* No snapshot was presented yet. */
    OT_EXPECT(optics_ui_snapshot_create(ui, &b) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_activate(ui, a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_last_presented_generation(ui) == optics_ui_snapshot_generation(a));
    response = build(ui, &in, 60, "new");
    OT_EXPECT(response.hovered); /* Input still targets the old visible geometry. */
    semantic_check old = {.expected = "old"};
    optics_ui_accessibility_walk(ui, visit, &old);
    OT_EXPECT(old.buttons == 1);
    OT_EXPECT_NEAR(old.bounds.x, original.x, .001f);

    OT_EXPECT(optics_ui_snapshot_activate(ui, b) == OPTICS_GFX_OK);
    response = build(ui, &in, 60, "new");
    OT_EXPECT(!response.hovered);
    OT_EXPECT(optics_ui_snapshot_activate(ui, a) == OPTICS_GFX_ERROR_INVALID_STATE);
    semantic_check current = {.expected = "new"};
    optics_ui_accessibility_walk(ui, visit, &current);
    OT_EXPECT(current.buttons == 1 && current.bounds.x > original.x + 30);

    optics_ui *foreign = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &foreign) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_activate(foreign, b) == OPTICS_GFX_ERROR_INVALID_STATE);
    optics_ui_release(foreign);

    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1,
                                                           .width = 400,
                                                           .height = 240},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color clear = 0;
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(a, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    uint32_t stride = 0;
    const uint8_t *pixels = optics_gfx_canvas_read_pixels(canvas, nullptr, nullptr, &stride);
    size_t bytes = (size_t)stride * 240;
    uint8_t *reference = malloc(bytes);
    OT_EXPECT(reference != nullptr && pixels != nullptr);
    memcpy(reference, pixels, bytes);
    optics_ui_release(ui);
    old.buttons = 0;
    optics_ui_snapshot_accessibility_walk(a, visit, &old);
    OT_EXPECT(old.buttons == 1);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(a, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    pixels = optics_gfx_canvas_read_pixels(canvas, nullptr, nullptr, &stride);
    OT_EXPECT(memcmp(reference, pixels, bytes) == 0);
    free(reference);
    optics_gfx_canvas_release(canvas);
    optics_ui_snapshot_release(a);
    optics_ui_snapshot_release(b);
}

static void capture_failure_is_not_partial_success(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = .016f};
    optics_ui_begin(ui, &in);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_CHECK, .size = 24});
    optics_ui_end(ui);
    /* Inject a compile failure through the canonical "frame could not be
     * compiled" flag (optics_ui__set_overflow), which is what arena exhaustion
     * ultimately surfaces as to compile_visuals. Icons no longer allocate from
     * the frame arena themselves — ADR-0112 bakes their coverage masks once
     * into a heap-backed device-resolution cache — so exhausting the arena
     * would no longer fail this frame, and the invariant under test is about
     * the failure propagating, not about which allocation tripped it. */
    ui->overflow = true;
    optics_ui_scene_snapshot *snapshot = (void *)(uintptr_t)1;
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
    OT_EXPECT(snapshot == nullptr);
    OT_EXPECT(optics_ui_last_presented_generation(ui) == 0);
    ui->overflow = false;
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    OT_EXPECT(snapshot != nullptr);
    optics_ui_snapshot_release(snapshot);
    optics_ui_release(ui);
}

static void unpublished_content_remains_dirty(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {400, 240}, .dt_seconds = .016f, .cursor = {-100, -100}};
    optics_ui_scene_snapshot *snapshot = nullptr;
    build(ui, &in, 0, "old");
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_activate(ui, snapshot) == OPTICS_GFX_OK);
    optics_ui_snapshot_release(snapshot);
    build(ui, &in, 0, "new");
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    optics_ui_snapshot_release(snapshot); /* Compiled, but presentation was dropped. */
    build(ui, &in, 0, "new");
    OT_EXPECT(optics_ui_frame_needs_repaint(ui));
    optics_ui_release(ui);
}

static optics_ui_node *build_scroll(optics_ui *ui, const optics_ui_input *input, float offset) {
    optics_ui_begin(ui, input);
    optics_ui_scroll_begin(
        ui, &(optics_ui_scroll_opts){.box = {.id = "scroll", .width = 200, .height = 80}});
    for (int i = 0; i < 20; i++) {
        optics_ui_push_id_int(ui, i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Item", .box = {.height = 20}});
        optics_ui_pop_id(ui);
    }
    optics_ui_scroll_end(ui);
    if (offset >= 0)
        optics_ui_scroll_to(ui, &(optics_ui_scroll_to_opts){.id = "scroll", .x = 0, .y = offset});
    optics_ui_end(ui);
    return optics_ui_node_first_child(optics_ui_root(ui));
}

static void scrollbar_input_uses_presented_geometry(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input input = {
        .display_size = {400, 240}, .dt_seconds = .016f, .cursor = {-100, -100}};
    optics_ui_node *node = build_scroll(ui, &input, 0);
    optics_ui_scene_snapshot *snapshot = nullptr;
    OT_EXPECT(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_activate(ui, snapshot) == OPTICS_GFX_OK);
    optics_ui_snapshot_release(snapshot);
    const optics_ui_scroll_state *initial = optics_ui_node_state(node, sizeof(*initial));
    float width = ui->theme.scrollbar_width;
    optics_ui_scroll_geometry visible = {
        .thumb = {node->final_rect.x + node->final_rect.w - width,
                  node->final_rect.y + initial->thumb_y, width, initial->thumb_h},
        .track_len = initial->track_len,
        .scroll_range = initial->scroll_range,
    };
    OT_EXPECT(visible.thumb.h > 0 && visible.track_len > 0);
    build_scroll(ui, &input, 200); /* Layout advances while the old thumb stays visible. */
    input.cursor = (optics_gfx_point){visible.thumb.x + visible.thumb.w / 2,
                                      visible.thumb.y + visible.thumb.h / 2};
    input.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    input.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    node = build_scroll(ui, &input, -1);
    optics_ui_scroll_state *state = optics_ui_node_state(node, sizeof(*state));
    OT_EXPECT(state->dragging);
    input.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    input.cursor.y += 5;
    node = build_scroll(ui, &input, -1);
    state = optics_ui_node_state(node, sizeof(*state));
    OT_EXPECT_NEAR(state->offset_y, 5 * visible.scroll_range / visible.track_len, .001f);
    optics_ui_release(ui);
}

int main(void) {
    delayed_presentation_and_owned_metadata();
    scrollbar_input_uses_presented_geometry();
    unpublished_content_remains_dirty();
    capture_failure_is_not_partial_success();
    return OT_REPORT();
}
