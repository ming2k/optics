/* test_state_style.c — widget state bits and per-instance styles (ADR-0058,
 * cascade amendments ADR-0061).
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* ---- state bits through synthetic frames ---------------------------- */

static void test_hover_and_press_bits(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    uint32_t hover_state = lens_get_response(ui).state;
    lens_end(ui);
    CHECK(hover_state & LENS_STATE_HOVERED);
    CHECK(!(hover_state & LENS_STATE_PRESSED));
    CHECK(!(hover_state & LENS_STATE_DISABLED));

    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    uint32_t press_state = lens_get_response(ui).state;
    lens_end(ui);
    CHECK((press_state & LENS_STATE_PRESSED) && (press_state & LENS_STATE_HOVERED));

    lens_destroy(ui);
}

static void test_disabled_bit_excludes_hover_and_press(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK", .box = {.disabled = true}});
    uint32_t state = lens_get_response(ui).state;
    lens_end(ui);
    CHECK(state & LENS_STATE_DISABLED);
    CHECK(!(state & (LENS_STATE_HOVERED | LENS_STATE_PRESSED | LENS_STATE_FOCUSED)));

    lens_destroy(ui);
}

static void test_focus_visible_is_keyboard_only(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish geometry and the tab order [A, B]. */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_end(ui);

    /* Frame 2: Tab moves focus to A during lens_end. */
    lens_input in = IN0;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_TAB, .pressed = true, .repeat = false};
    in.key_count = 1;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_end(ui);

    /* Frame 3: A reports FOCUSED|FOCUS_VISIBLE (keyboard modality). */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    uint32_t a_state = lens_get_response(ui).state;
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_end(ui);
    CHECK(a_state & LENS_STATE_FOCUSED);
    CHECK(a_state & LENS_STATE_FOCUS_VISIBLE);

    /* Frame 4: a pointer press on B focuses it without FOCUS_VISIBLE. */
    in = IN0;
    in.cursor = (flux_point){20, 50};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    uint32_t b_state = lens_get_response(ui).state;
    lens_end(ui);
    CHECK(b_state & LENS_STATE_FOCUSED);
    CHECK(!(b_state & LENS_STATE_FOCUS_VISIBLE));
    CHECK(b_state & LENS_STATE_PRESSED);

    lens_destroy(ui);
}

static void test_widget_owned_bits(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool on = true;
    lens_begin(ui, &IN0);
    lens_selectable(ui, &(lens_selectable_opts){.label = "Sel", .selected = true});
    uint32_t sel = lens_get_response(ui).state;
    lens_selectable(ui, &(lens_selectable_opts){.label = "Unsel", .selected = false});
    uint32_t unsel = lens_get_response(ui).state;
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "On", .value = &on});
    uint32_t toggle = lens_get_response(ui).state;
    lens_end(ui);

    CHECK(sel & LENS_STATE_SELECTED);
    CHECK(!(unsel & LENS_STATE_SELECTED));
    CHECK(toggle & LENS_STATE_ACTIVE);

    lens_destroy(ui);
}

/* ---- resolver: fallback, override, derivation, dim order ------------ */

static void test_resolver_null_instance_is_verbatim_theme(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    lens_style_resolved r = lensi_style_resolve(ui, NULL, &t,
                                                LENS_STATE_HOVERED | LENS_STATE_PRESSED |
                                                    LENS_STATE_FOCUSED | LENS_STATE_DISABLED);
    CHECK(r.bg == t.color_bg);
    CHECK(r.bg_hover == t.color_hover);
    CHECK(r.bg_pressed == t.color_active);
    CHECK(r.fg == t.color_fg);
    CHECK(r.border == t.color_border);
    CHECK(r.accent == t.color_accent);
    CHECK(r.disabled == t.color_disabled);
    CHECK_NEAR(r.corner_radius, t.corner_radius, 0.0f);
    CHECK_NEAR(r.border_width, t.border_width, 0.0f);
    CHECK_NEAR(r.padding, t.padding, 0.0f);
    CHECK_NEAR(r.gap, t.gap, 0.0f);
    CHECK_NEAR(r.font_size, t.font_size, 0.0f);

    lens_destroy(ui);
}

static void test_resolver_override_and_derivation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG | LENS_STYLE_PADDING;
    inst.bg = red;
    inst.padding = 3.0f;

    lens_style_resolved r = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    CHECK(r.bg == red);
    CHECK(r.fg == t.color_fg);
    CHECK(r.accent == t.color_accent);
    CHECK_NEAR(r.padding, 3.0f, 0.0f);
    CHECK_NEAR(r.gap, t.gap, 0.0f);
    CHECK(r.bg_hover == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_HOVER_LIFT));
    CHECK(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));
    CHECK(r.bg_hover != t.color_hover);

    flux_color green = flux_color_rgba(0x20, 0xc0, 0x20, 0xff);
    inst.fields |= LENS_STYLE_BG_HOVER;
    inst.bg_hover = green;
    r = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    CHECK(r.bg_hover == green);
    CHECK(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));

    lens_destroy(ui);
}

static void test_resolver_disabled_dims_last(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG;
    inst.bg = red;

    lens_style_resolved en = lensi_style_resolve(ui, &inst, &t, 0);
    lens_style_resolved dis = lensi_style_resolve(
        ui, &inst, &t, LENS_STATE_DISABLED | LENS_STATE_HOVERED | LENS_STATE_PRESSED);

    CHECK(dis.bg == lensi_lerp_color(en.bg, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    CHECK(dis.bg_hover ==
          lensi_lerp_color(en.bg_hover, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    CHECK(dis.bg_pressed ==
          lensi_lerp_color(en.bg_pressed, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    CHECK(dis.fg == t.color_fg);
    CHECK(dis.accent == t.color_accent);

    lens_style_resolved a = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    lens_style_resolved b = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    CHECK(inst.fields == LENS_STYLE_BG);
    CHECK(inst.bg == red);

    lens_destroy(ui);
}

int main(void) {
    test_hover_and_press_bits();
    test_disabled_bit_excludes_hover_and_press();
    test_focus_visible_is_keyboard_only();
    test_widget_owned_bits();
    test_resolver_null_instance_is_verbatim_theme();
    test_resolver_override_and_derivation();
    test_resolver_disabled_dims_last();
    return TEST_REPORT();
}
