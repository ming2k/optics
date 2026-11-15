/* test_state_style.c — widget state bits and per-instance styles (ADR-0058,
 * cascade amendments ADR-0061).
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* ---- state bits through synthetic frames ---------------------------- */

static void test_hover_and_press_bits(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_test_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    uint32_t hover_state = lens_get_response(ui).state;
    lens_test_end(ui);
    OT_EXPECT(hover_state & LENS_STATE_HOVERED);
    OT_EXPECT(!(hover_state & LENS_STATE_PRESSED));
    OT_EXPECT(!(hover_state & LENS_STATE_DISABLED));

    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    uint32_t press_state = lens_get_response(ui).state;
    lens_test_end(ui);
    OT_EXPECT((press_state & LENS_STATE_PRESSED) && (press_state & LENS_STATE_HOVERED));

    lens_release(ui);
}

static void test_disabled_bit_excludes_hover_and_press(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_test_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "OK", .box = {.disabled = true}});
    uint32_t state = lens_get_response(ui).state;
    lens_test_end(ui);
    OT_EXPECT(state & LENS_STATE_DISABLED);
    OT_EXPECT(!(state & (LENS_STATE_HOVERED | LENS_STATE_PRESSED | LENS_STATE_FOCUSED)));

    lens_release(ui);
}

static void test_focus_visible_is_keyboard_only(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    /* Frame 1: establish geometry and the tab order [A, B]. */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_test_end(ui);

    /* Frame 2: Tab moves focus to A during lens_end. */
    lens_input in = IN0;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_TAB, .pressed = true, .repeat = false};
    in.key_count = 1;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_test_end(ui);

    /* Frame 3: A reports FOCUSED|FOCUS_VISIBLE (keyboard modality). */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    uint32_t a_state = lens_get_response(ui).state;
    lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_test_end(ui);
    OT_EXPECT(a_state & LENS_STATE_FOCUSED);
    OT_EXPECT(a_state & LENS_STATE_FOCUS_VISIBLE);

    /* Frame 4: a pointer press on B focuses it without FOCUS_VISIBLE. */
    in = IN0;
    in.cursor = (flux_point){20, 50};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_button(ui, &(lens_button_opts){.label = "B"});
    uint32_t b_state = lens_get_response(ui).state;
    lens_test_end(ui);
    OT_EXPECT(b_state & LENS_STATE_FOCUSED);
    OT_EXPECT(!(b_state & LENS_STATE_FOCUS_VISIBLE));
    OT_EXPECT(b_state & LENS_STATE_PRESSED);

    lens_release(ui);
}

static void test_widget_owned_bits(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    bool on = true;
    lens_begin(ui, &IN0);
    lens_selectable(ui, &(lens_selectable_opts){.label = "Sel", .selected = true});
    uint32_t sel = lens_get_response(ui).state;
    lens_selectable(ui, &(lens_selectable_opts){.label = "Unsel", .selected = false});
    uint32_t unsel = lens_get_response(ui).state;
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "On", .value = &on});
    uint32_t toggle = lens_get_response(ui).state;
    lens_test_end(ui);

    OT_EXPECT(sel & LENS_STATE_SELECTED);
    OT_EXPECT(!(unsel & LENS_STATE_SELECTED));
    OT_EXPECT(toggle & LENS_STATE_ACTIVE);

    lens_release(ui);
}

/* ---- resolver: fallback, override, derivation, dim order ------------ */

static void test_resolver_null_instance_is_verbatim_theme(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    lens_style_resolved r = lensi_style_resolve(ui, nullptr, &t,
                                                LENS_STATE_HOVERED | LENS_STATE_PRESSED |
                                                    LENS_STATE_FOCUSED | LENS_STATE_DISABLED);
    OT_EXPECT(r.bg == t.color_bg);
    OT_EXPECT(r.bg_hover == t.color_hover);
    OT_EXPECT(r.bg_pressed == t.color_active);
    OT_EXPECT(r.fg == t.color_fg);
    OT_EXPECT(r.border == t.color_border);
    OT_EXPECT(r.accent == t.color_accent);
    OT_EXPECT(r.disabled == t.color_disabled);
    OT_EXPECT_NEAR(r.corner_radius, t.corner_radius, 0.0f);
    OT_EXPECT_NEAR(r.border_width, t.border_width, 0.0f);
    OT_EXPECT_NEAR(r.padding, t.padding, 0.0f);
    OT_EXPECT_NEAR(r.gap, t.gap, 0.0f);
    OT_EXPECT_NEAR(r.font_size, t.font_size, 0.0f);

    lens_release(ui);
}

static void test_resolver_override_and_derivation(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG | LENS_STYLE_PADDING;
    inst.bg = red;
    inst.padding = 3.0f;

    lens_style_resolved r = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    OT_EXPECT(r.bg == red);
    OT_EXPECT(r.fg == t.color_fg);
    OT_EXPECT(r.accent == t.color_accent);
    OT_EXPECT_NEAR(r.padding, 3.0f, 0.0f);
    OT_EXPECT_NEAR(r.gap, t.gap, 0.0f);
    OT_EXPECT(r.bg_hover == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_HOVER_LIFT));
    OT_EXPECT(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));
    OT_EXPECT(r.bg_hover != t.color_hover);

    flux_color green = flux_color_rgba(0x20, 0xc0, 0x20, 0xff);
    inst.fields |= LENS_STYLE_BG_HOVER;
    inst.bg_hover = green;
    r = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    OT_EXPECT(r.bg_hover == green);
    OT_EXPECT(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));

    lens_release(ui);
}

static void test_resolver_disabled_dims_last(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG;
    inst.bg = red;

    lens_style_resolved en = lensi_style_resolve(ui, &inst, &t, 0);
    lens_style_resolved dis = lensi_style_resolve(
        ui, &inst, &t, LENS_STATE_DISABLED | LENS_STATE_HOVERED | LENS_STATE_PRESSED);

    OT_EXPECT(dis.bg == lensi_lerp_color(en.bg, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.bg_hover ==
              lensi_lerp_color(en.bg_hover, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.bg_pressed ==
              lensi_lerp_color(en.bg_pressed, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.fg == t.color_fg);
    OT_EXPECT(dis.accent == t.color_accent);

    lens_style_resolved a = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    lens_style_resolved b = lensi_style_resolve(ui, &inst, &t, LENS_STATE_HOVERED);
    OT_EXPECT(memcmp(&a, &b, sizeof a) == 0);
    OT_EXPECT(inst.fields == LENS_STYLE_BG);
    OT_EXPECT(inst.bg == red);

    lens_release(ui);
}

static bool has_border_cmd(const lens_node *n) {
    if (!n)
        return false;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == LENS_DRAW_BORDER)
            return true;
    }
    return false;
}

static void test_minimalist_theme_defaults_and_borders(void) {
    lens_theme def = lens_theme_default();
    lens_theme dark = lens_theme_dark();
    lens_theme classic_light = lens_theme_classic(false);
    lens_theme classic_dark = lens_theme_classic(true);
    lens_theme mat = lens_theme_for_material(LENS_MATERIAL_LIQUID_GLASS, true);

    OT_EXPECT_NEAR(def.border_width, 0.0f, 0.0f);
    OT_EXPECT_NEAR(dark.border_width, 0.0f, 0.0f);
    OT_EXPECT_NEAR(classic_light.border_width, 1.0f, 0.0f);
    OT_EXPECT_NEAR(classic_dark.border_width, 1.0f, 0.0f);
    OT_EXPECT_NEAR(mat.border_width, 1.0f, 0.0f);

    /* Semantic surface tokens (ADR-0105) */
    OT_EXPECT(def.color_surface != 0);
    OT_EXPECT(def.color_surface_sunken != 0);
    OT_EXPECT(def.color_surface_elevated != 0);
    OT_EXPECT(dark.color_surface != 0);
    OT_EXPECT(dark.color_surface_sunken != 0);
    OT_EXPECT(dark.color_surface_elevated != 0);

    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: default theme (minimalist, border_width = 0) */
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "btn1"});
    bool chk = false;
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "chk1", .value = &chk});
    char text_buf[32] = "hello";
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    lens_test_end(ui);

    lens_node *n_btn = lens_find(ui, lens_current_id(ui, "btn1"));
    lens_node *n_chk = lens_find(ui, lens_current_id(ui, "chk1"));
    lens_node *n_tf = lens_find(ui, lens_current_id(ui, "tf1"));

    OT_EXPECT(n_btn != nullptr);
    OT_EXPECT(!has_border_cmd(n_btn));
    OT_EXPECT(n_chk != nullptr);
    OT_EXPECT(!has_border_cmd(n_chk));
    OT_EXPECT(n_tf != nullptr);
    OT_EXPECT(!has_border_cmd(n_tf));

    /* Frame 2: classic theme (border_width = 1.0) */
    lens_set_theme(ui, classic_dark);
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "btn1"});
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "chk1", .value = &chk});
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    lens_test_end(ui);

    n_btn = lens_find(ui, lens_current_id(ui, "btn1"));
    n_chk = lens_find(ui, lens_current_id(ui, "chk1"));
    n_tf = lens_find(ui, lens_current_id(ui, "tf1"));

    OT_EXPECT(n_btn != nullptr && has_border_cmd(n_btn));
    OT_EXPECT(n_chk != nullptr && has_border_cmd(n_chk));
    OT_EXPECT(n_tf != nullptr && has_border_cmd(n_tf));

    /* Frame 3: default theme with focused textedit -> emits focus outline */
    lens_set_theme(ui, dark);
    lens_set_focus(ui, lens_current_id(ui, "tf1"));
    lens_begin(ui, &in);
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    lens_test_end(ui);

    n_tf = lens_find(ui, lens_current_id(ui, "tf1"));
    OT_EXPECT(n_tf != nullptr && has_border_cmd(n_tf));

    lens_release(ui);
}

int main(void) {
    test_hover_and_press_bits();
    test_disabled_bit_excludes_hover_and_press();
    test_focus_visible_is_keyboard_only();
    test_widget_owned_bits();
    test_resolver_null_instance_is_verbatim_theme();
    test_resolver_override_and_derivation();
    test_resolver_disabled_dims_last();
    test_minimalist_theme_defaults_and_borders();
    return OT_REPORT();
}
