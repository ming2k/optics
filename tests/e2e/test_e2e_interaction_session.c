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

#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

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
    lens *ui;
    app_state state;
    lens_response button;
    lens_response checkbox;
    lens_response name_field;
    uint32_t frames;
} session;

static void build(session *s) {
    lens_label(s->ui, &(lens_label_opts){.text = "Optics E2E session"});
    lens_row_begin(s->ui, nullptr);
    s->button =
        lens_button(s->ui, &(lens_button_opts){.box = {.id = "new"}, .label = "New document"});
    if (s->button.clicked)
        s->state.clicks++;
    s->checkbox = lens_checkbox(
        s->ui,
        &(lens_checkbox_opts){.box = {.id = "wrap"}, .label = "Wrap", .value = &s->state.wrap});
    lens_flex(s->ui, 1.0f);
    lens_slider(s->ui, &(lens_slider_opts){
                           .label = "Zoom", .value = &s->state.zoom, .min = 0.5f, .max = 4.0f});
    lens_row_end(s->ui);

    s->name_field = lens_textedit(s->ui, &(lens_textedit_opts){.box = {.id = "name"},
                                                               .buf = s->state.name,
                                                               .cap = sizeof s->state.name,
                                                               .placeholder = "Document name"});
}

static void frame(session *s, lens_input input) {
    input.display_size = (flux_point){DISPLAY_W, DISPLAY_H};
    input.dt_seconds = 1.0f / 60.0f;
    lens_begin(s->ui, &input);
    build(s);
    lens_test_end(s->ui);
    s->frames++;
    OT_EXPECT(!lens_overflowed(s->ui));
    OT_EXPECT(!lens_has_duplicate_ids(s->ui));
}

static flux_point centre_of(session *s, lens_id id) {
    flux_rect rect = lens_node_bounds(lens_find(s->ui, id));
    return (flux_point){rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
}

static void click(session *s) {
    flux_point at = centre_of(s, s->button.id);
    lens_input press = {
        .cursor = at, .mouse_down[LENS_MOUSE_LEFT] = true, .mouse_pressed[LENS_MOUSE_LEFT] = true};
    frame(s, press);

    lens_input release = {.cursor = at, .mouse_released[LENS_MOUSE_LEFT] = true};
    frame(s, release);
}

static void toggle_checkbox(session *s) {
    flux_point at = centre_of(s, s->checkbox.id);
    lens_input press = {
        .cursor = at, .mouse_down[LENS_MOUSE_LEFT] = true, .mouse_pressed[LENS_MOUSE_LEFT] = true};
    frame(s, press);

    lens_input release = {.cursor = at, .mouse_released[LENS_MOUSE_LEFT] = true};
    frame(s, release);
}

static void type_name(session *s) {
    lens_set_focus(s->ui, s->name_field.id);
    lens_input typing = {};
    strncpy(typing.text_utf8, "atlas", sizeof typing.text_utf8 - 1);
    frame(s, typing);
}

static uint32_t count_a11y = 0;
static bool saw_button = false;
static bool saw_text = false;

static void visit(const lens_semantics *semantics, flux_rect bounds, lens_id id, lens_id parent,
                  void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    (void)user;
    count_a11y++;
    if (semantics->role == LENS_ROLE_BUTTON && semantics->name &&
        strcmp(semantics->name, "New document") == 0)
        saw_button = true;
    if (semantics->role == LENS_ROLE_TEXTFIELD && semantics->value &&
        strcmp(semantics->value, "atlas") == 0)
        saw_text = true;
}

int main(void) {
    session s = {};
    OT_REQUIRE(lens_create(&(lens_desc){.theme = lens_theme_dark()}, &s.ui) == FLUX_OK);

    /* Frame 0: first layout. Nothing has been clicked. */
    frame(&s, (lens_input){});
    flux_rect button_rect = lens_node_bounds(lens_find(s.ui, s.button.id));
    OT_EXPECT(button_rect.w > 0.0f && button_rect.h > 0.0f);
    OT_EXPECT(s.state.clicks == 0);

    click(&s);
    OT_EXPECT(s.state.clicks == 1);
    OT_EXPECT(s.button.hovered);

    toggle_checkbox(&s);
    OT_EXPECT(s.state.wrap == true);

    type_name(&s);
    OT_EXPECT_STR_EQ(s.state.name, "atlas");
    OT_EXPECT(s.name_field.state & LENS_STATE_FOCUSED);

    /* A stable frame must have a published, activated snapshot. */
    const uint64_t generation = lens_generation(s.ui);
    OT_EXPECT(generation > 0);
    OT_EXPECT(lens_last_presented_generation(s.ui) == generation);

    lens_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(lens_snapshot_create(s.ui, &snapshot) == FLUX_OK);
    OT_REQUIRE(lens_snapshot_display_list(snapshot) != nullptr);
    OT_EXPECT(flux_display_list_command_count(lens_snapshot_display_list(snapshot)) > 0);

    count_a11y = 0;
    saw_button = false;
    saw_text = false;
    lens_snapshot_accessibility_walk(snapshot, visit, nullptr);
    OT_EXPECT(count_a11y > 0);
    OT_EXPECT(saw_button);
    OT_EXPECT(saw_text);

    flux_canvas *canvas = nullptr;
    OT_REQUIRE(flux_canvas_create_cpu((uint32_t)DISPLAY_W, (uint32_t)DISPLAY_H, 1.0f, &canvas) ==
               FLUX_OK);
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(
        flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                           .clear_color = &clear}) == FLUX_OK);
    OT_EXPECT(lens_snapshot_submit(snapshot, canvas) == FLUX_OK);
    OT_REQUIRE(flux_canvas_end(canvas) == FLUX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *pixels = flux_canvas_read_pixels(canvas, &width, &height, &stride);
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

    flux_canvas_release(canvas);
    lens_snapshot_release(snapshot);
    lens_release(s.ui);
    return OT_REPORT();
}
