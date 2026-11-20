/*
 * e2e_interaction_session — a real host loop without a window.
 *
 * One synthetic session drives the public API the way iris does: per-frame
 * input, a build callback, lens_end, snapshot publication, activation, and a
 * CPU replay. It spans pointer interaction, focus/keyboard input, text, an
 * accessibility walk, and pixel output.
 *
 * This is deliberately not a widget unit test. A regression in the frame
 * lifecycle, input folding, snapshot handshake, or replay path breaks the
 * session even if every individual widget still passes its own test.
 */

#include "lens_test.h"
#include "optics_test.h"

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

#include <string.h>

#define DISPLAY_W 420.0f
#define DISPLAY_H 220.0f

typedef struct app_state {
    int clicks;
    bool wrap;
    float zoom;
    char name[64];
} app_state;

typedef struct session {
    optics_ui *ui;
    app_state state;
    optics_ui_response button;
    optics_ui_response checkbox;
    optics_ui_response name_field;
    uint32_t frames;
} session;

static void build(session *s) {
    optics_ui_label(s->ui, &(optics_ui_label_opts){.text = "Optics E2E session"});
    optics_ui_row_begin(s->ui, nullptr);
    s->button = optics_ui_button(
        s->ui, &(optics_ui_button_opts){.box = {.id = "new"}, .label = "New document"});
    if (s->button.clicked)
        s->state.clicks++;
    s->checkbox = optics_ui_checkbox(s->ui, &(optics_ui_checkbox_opts){.box = {.id = "wrap"},
                                                                       .label = "Wrap",
                                                                       .value = &s->state.wrap});
    optics_ui_flex(s->ui, 1.0f);
    optics_ui_slider(s->ui,
                     &(optics_ui_slider_opts){
                         .label = "Zoom", .value = &s->state.zoom, .min = 0.5f, .max = 4.0f});
    optics_ui_row_end(s->ui);

    s->name_field =
        optics_ui_textedit(s->ui, &(optics_ui_textedit_opts){.box = {.id = "name"},
                                                             .buf = s->state.name,
                                                             .cap = sizeof s->state.name,
                                                             .placeholder = "Document name"});
}

static void frame(session *s, optics_ui_input input) {
    input.display_size = (optics_gfx_point){DISPLAY_W, DISPLAY_H};
    input.dt_seconds = 1.0f / 60.0f;
    optics_ui_begin(s->ui, &input);
    build(s);
    optics_ui_test_end(s->ui);
    s->frames++;
    OT_EXPECT(!optics_ui_overflowed(s->ui));
    OT_EXPECT(!optics_ui_has_duplicate_ids(s->ui));
}

static optics_gfx_point centre_of(session *s, optics_ui_id id) {
    optics_gfx_rect rect = optics_ui_node_bounds(optics_ui_find(s->ui, id));
    return (optics_gfx_point){rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
}

static void click(session *s) {
    optics_gfx_point at = centre_of(s, s->button.id);
    optics_ui_input press = {.cursor = at,
                             .mouse_down[OPTICS_UI_MOUSE_LEFT] = true,
                             .mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true};
    frame(s, press);

    optics_ui_input release = {.cursor = at, .mouse_released[OPTICS_UI_MOUSE_LEFT] = true};
    frame(s, release);
}

static void toggle_checkbox(session *s) {
    optics_gfx_point at = centre_of(s, s->checkbox.id);
    optics_ui_input press = {.cursor = at,
                             .mouse_down[OPTICS_UI_MOUSE_LEFT] = true,
                             .mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true};
    frame(s, press);

    optics_ui_input release = {.cursor = at, .mouse_released[OPTICS_UI_MOUSE_LEFT] = true};
    frame(s, release);
}

static void type_name(session *s) {
    optics_ui_set_focus(s->ui, s->name_field.id);
    optics_ui_input typing = {};
    strncpy(typing.text_utf8, "atlas", sizeof typing.text_utf8 - 1);
    frame(s, typing);
}

static uint32_t count_a11y = 0;
static bool saw_button = false;
static bool saw_text = false;

static void visit(const optics_ui_semantics *semantics, optics_gfx_rect bounds, optics_ui_id id,
                  optics_ui_id parent, void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    (void)user;
    count_a11y++;
    if (semantics->role == OPTICS_UI_ROLE_BUTTON && semantics->name &&
        strcmp(semantics->name, "New document") == 0)
        saw_button = true;
    if (semantics->role == OPTICS_UI_ROLE_TEXTFIELD && semantics->value &&
        strcmp(semantics->value, "atlas") == 0)
        saw_text = true;
}

int main(void) {
    session s = {};
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark()}, &s.ui) ==
               OPTICS_GFX_OK);

    /* Frame 0: first layout. Nothing has been clicked. */
    frame(&s, (optics_ui_input){});
    optics_gfx_rect button_rect = optics_ui_node_bounds(optics_ui_find(s.ui, s.button.id));
    OT_EXPECT(button_rect.w > 0.0f && button_rect.h > 0.0f);
    OT_EXPECT(s.state.clicks == 0);

    click(&s);
    OT_EXPECT(s.state.clicks == 1);
    OT_EXPECT(s.button.hovered);

    toggle_checkbox(&s);
    OT_EXPECT(s.state.wrap == true);

    type_name(&s);
    OT_EXPECT_STR_EQ(s.state.name, "atlas");
    OT_EXPECT(s.name_field.state & OPTICS_UI_STATE_FOCUSED);

    /* A stable frame must have a published, activated snapshot. */
    const uint64_t generation = optics_ui_generation(s.ui);
    OT_EXPECT(generation > 0);
    OT_EXPECT(optics_ui_last_presented_generation(s.ui) == generation);

    optics_ui_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(optics_ui_snapshot_create(s.ui, &snapshot) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_ui_snapshot_display_list(snapshot) != nullptr);
    OT_EXPECT(optics_gfx_display_list_command_count(optics_ui_snapshot_display_list(snapshot)) > 0);

    count_a11y = 0;
    saw_button = false;
    saw_text = false;
    optics_ui_snapshot_accessibility_walk(snapshot, visit, nullptr);
    OT_EXPECT(count_a11y > 0);
    OT_EXPECT(saw_button);
    OT_EXPECT(saw_text);

    optics_gfx_canvas *canvas = nullptr;
    OT_REQUIRE(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = (uint32_t)DISPLAY_W,
                                                           .height = (uint32_t)DISPLAY_H},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(snapshot, canvas) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *pixels = optics_gfx_canvas_read_pixels(canvas, &width, &height, &stride);
    OT_REQUIRE(pixels != nullptr);
    OT_EXPECT(width == (uint32_t)DISPLAY_W && height == (uint32_t)DISPLAY_H);

    size_t lit = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4u;
            if (p[0] + p[1] + p[2] > 30)
                lit++;
        }
    }
    OT_EXPECT(lit > 1000);

    optics_gfx_canvas_release(canvas);
    optics_ui_snapshot_release(snapshot);
    optics_ui_release(s.ui);
    return OT_REPORT();
}
