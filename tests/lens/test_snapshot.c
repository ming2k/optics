/* Publication, presentation and failure semantics through the public snapshot
 * API. Internal access is used only to inject scratch-allocation failure. */
#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/icon.h>
#include <stdlib.h>
#include <string.h>

static lens_response build(lens *ui, const lens_input *in, float pad, const char *label) {
    lens_begin(ui, in);
    lens_row_begin(ui, &(lens_layout_opts){.box = {.id = "panel"}, .pad = pad});
    lens_response result =
        lens_button(ui, &(lens_button_opts){.label = label,
                                            .box = {.id = "action", .width = 80, .height = 30}});
    lens_row_end(ui);
    lens_end(ui); /* Deliberately do not activate the newly built geometry. */
    return result;
}

typedef struct {
    const char *expected;
    flux_rect bounds;
    unsigned buttons;
} semantic_check;

static void visit(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                  void *user) {
    semantic_check *check = user;
    if (s->role != LENS_ROLE_BUTTON)
        return;
    CHECK(s->name && strcmp(s->name, check->expected) == 0);
    check->bounds = bounds;
    check->buttons++;
}

static void delayed_presentation_and_owned_metadata(void) {
    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 240}, .dt_seconds = .016f};
    lens_response response = build(ui, &in, 0, "old");
    flux_rect original = lens_node_bounds(lens_find(ui, response.id));
    in.cursor = (flux_point){original.x + 5, original.y + 5};
    lens_scene_snapshot *a = nullptr, *b = nullptr;
    CHECK(lens_snapshot_create(ui, &a) == FLUX_OK);
    CHECK(lens_last_presented_generation(ui) == 0);

    response = build(ui, &in, 60, "new");
    CHECK(!response.hovered); /* No snapshot was presented yet. */
    CHECK(lens_snapshot_create(ui, &b) == FLUX_OK);
    CHECK(lens_snapshot_activate(ui, a) == FLUX_OK);
    CHECK(lens_last_presented_generation(ui) == lens_snapshot_generation(a));
    response = build(ui, &in, 60, "new");
    CHECK(response.hovered); /* Input still targets the old visible geometry. */
    semantic_check old = {.expected = "old"};
    lens_accessibility_walk(ui, visit, &old);
    CHECK(old.buttons == 1);
    CHECK_NEAR(old.bounds.x, original.x, .001f);

    CHECK(lens_snapshot_activate(ui, b) == FLUX_OK);
    response = build(ui, &in, 60, "new");
    CHECK(!response.hovered);
    CHECK(lens_snapshot_activate(ui, a) == FLUX_ERROR_INVALID_STATE);
    semantic_check current = {.expected = "new"};
    lens_accessibility_walk(ui, visit, &current);
    CHECK(current.buttons == 1 && current.bounds.x > original.x + 30);

    lens *foreign = nullptr;
    CHECK(lens_create(&(lens_desc){}, &foreign) == FLUX_OK);
    CHECK(lens_snapshot_activate(foreign, b) == FLUX_ERROR_INVALID_STATE);
    lens_release(foreign);

    flux_canvas *canvas = nullptr;
    CHECK(flux_canvas_create_cpu(400, 240, 1, &canvas) == FLUX_OK);
    flux_color clear = 0;
    CHECK(flux_canvas_begin(canvas, nullptr, &clear) == FLUX_OK);
    CHECK(lens_snapshot_submit(a, canvas) == FLUX_OK);
    CHECK(flux_canvas_end(canvas) == FLUX_OK);
    uint32_t stride = 0;
    const uint8_t *pixels = flux_canvas_read_pixels(canvas, nullptr, nullptr, &stride);
    size_t bytes = (size_t)stride * 240;
    uint8_t *reference = malloc(bytes);
    CHECK(reference != nullptr && pixels != nullptr);
    memcpy(reference, pixels, bytes);
    lens_release(ui);
    old.buttons = 0;
    lens_snapshot_accessibility_walk(a, visit, &old);
    CHECK(old.buttons == 1);
    CHECK(flux_canvas_begin(canvas, nullptr, &clear) == FLUX_OK);
    CHECK(lens_snapshot_submit(a, canvas) == FLUX_OK);
    CHECK(flux_canvas_end(canvas) == FLUX_OK);
    pixels = flux_canvas_read_pixels(canvas, nullptr, nullptr, &stride);
    CHECK(memcmp(reference, pixels, bytes) == 0);
    free(reference);
    flux_canvas_release(canvas);
    lens_snapshot_release(a);
    lens_snapshot_release(b);
}

static void capture_failure_is_not_partial_success(void) {
    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = .016f};
    lens_begin(ui, &in);
    lens_icon(ui, &(lens_icon_opts){.id = LENS_ICON_CHECK, .size = 24});
    lens_end(ui);
    size_t mark = ui->arena.used;
    ui->arena.used = ui->arena.capacity; /* Fail path scratch allocation. */
    lens_scene_snapshot *snapshot = (void *)(uintptr_t)1;
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_ERROR_OUT_OF_MEMORY);
    CHECK(snapshot == nullptr);
    CHECK(ui->arena.used == ui->arena.capacity);
    CHECK(lens_last_presented_generation(ui) == 0);
    ui->arena.used = mark;
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    CHECK(snapshot != nullptr);
    lens_snapshot_release(snapshot);
    lens_release(ui);
}

static void unpublished_content_remains_dirty(void) {
    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 240}, .dt_seconds = .016f, .cursor = {-100, -100}};
    lens_scene_snapshot *snapshot = nullptr;
    build(ui, &in, 0, "old");
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    CHECK(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    lens_snapshot_release(snapshot);
    build(ui, &in, 0, "new");
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    lens_snapshot_release(snapshot); /* Compiled, but presentation was dropped. */
    build(ui, &in, 0, "new");
    CHECK(lens_frame_needs_repaint(ui));
    lens_release(ui);
}

static lens_node *build_scroll(lens *ui, const lens_input *input, float offset) {
    lens_begin(ui, input);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "scroll", .width = 200, .height = 80}});
    for (int i = 0; i < 20; i++) {
        lens_push_id_int(ui, i);
        lens_label(ui, &(lens_label_opts){.text = "Item", .box = {.height = 20}});
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
    if (offset >= 0)
        lens_scroll_to(ui, "scroll", 0, offset);
    lens_end(ui);
    return lens_node_first_child(lens_root(ui));
}

static void scrollbar_input_uses_presented_geometry(void) {
    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input input = {.display_size = {400, 240}, .dt_seconds = .016f, .cursor = {-100, -100}};
    lens_node *node = build_scroll(ui, &input, 0);
    lens_scene_snapshot *snapshot = nullptr;
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    CHECK(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    lens_snapshot_release(snapshot);
    const lens_scroll_state *initial = lens_node_state(node, sizeof(*initial));
    float width = ui->theme.scrollbar_width;
    lens_scroll_geometry visible = {
        .thumb = {node->final_rect.x + node->final_rect.w - width,
                  node->final_rect.y + initial->thumb_y, width, initial->thumb_h},
        .track_len = initial->track_len,
        .scroll_range = initial->scroll_range,
    };
    CHECK(visible.thumb.h > 0 && visible.track_len > 0);
    build_scroll(ui, &input, 200); /* Layout advances while the old thumb stays visible. */
    input.cursor =
        (flux_point){visible.thumb.x + visible.thumb.w / 2, visible.thumb.y + visible.thumb.h / 2};
    input.mouse_down[LENS_MOUSE_LEFT] = true;
    input.mouse_pressed[LENS_MOUSE_LEFT] = true;
    node = build_scroll(ui, &input, -1);
    lens_scroll_state *state = lens_node_state(node, sizeof(*state));
    CHECK(state->dragging);
    input.mouse_pressed[LENS_MOUSE_LEFT] = false;
    input.cursor.y += 5;
    node = build_scroll(ui, &input, -1);
    state = lens_node_state(node, sizeof(*state));
    CHECK_NEAR(state->offset_y, 5 * visible.scroll_range / visible.track_len, .001f);
    lens_release(ui);
}

int main(void) {
    delayed_presentation_and_owned_metadata();
    scrollbar_input_uses_presented_geometry();
    unpublished_content_remains_dirty();
    capture_failure_is_not_partial_success();
    return TEST_REPORT();
}
