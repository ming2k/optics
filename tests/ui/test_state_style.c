/* test_state_style.c — widget state bits and per-instance styles (ADR-0058,
 * cascade amendments ADR-0061).
 */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* ---- state bits through synthetic frames ---------------------------- */

static void test_hover_and_press_bits(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    uint32_t hover_state = optics_ui_get_response(ui).state;
    optics_ui_test_end(ui);
    OT_EXPECT(hover_state & OPTICS_UI_STATE_HOVERED);
    OT_EXPECT(!(hover_state & OPTICS_UI_STATE_PRESSED));
    OT_EXPECT(!(hover_state & OPTICS_UI_STATE_DISABLED));

    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    uint32_t press_state = optics_ui_get_response(ui).state;
    optics_ui_test_end(ui);
    OT_EXPECT((press_state & OPTICS_UI_STATE_PRESSED) && (press_state & OPTICS_UI_STATE_HOVERED));

    optics_ui_release(ui);
}

static void test_disabled_bit_excludes_hover_and_press(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK", .box = {.disabled = true}});
    uint32_t state = optics_ui_get_response(ui).state;
    optics_ui_test_end(ui);
    OT_EXPECT(state & OPTICS_UI_STATE_DISABLED);
    OT_EXPECT(
        !(state & (OPTICS_UI_STATE_HOVERED | OPTICS_UI_STATE_PRESSED | OPTICS_UI_STATE_FOCUSED)));

    optics_ui_release(ui);
}

static void test_focus_visible_is_keyboard_only(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Frame 1: establish geometry and the tab order [A, B]. */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_test_end(ui);

    /* Frame 2: Tab moves focus to A during optics_ui_end. */
    optics_ui_input in = IN0;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_TAB, .pressed = true, .repeat = false};
    in.key_count = 1;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_test_end(ui);

    /* Frame 3: A reports FOCUSED|FOCUS_VISIBLE (keyboard modality). */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    uint32_t a_state = optics_ui_get_response(ui).state;
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_test_end(ui);
    OT_EXPECT(a_state & OPTICS_UI_STATE_FOCUSED);
    OT_EXPECT(a_state & OPTICS_UI_STATE_FOCUS_VISIBLE);

    /* Frame 4: a pointer press on B focuses it without FOCUS_VISIBLE. */
    in = IN0;
    in.cursor = (optics_gfx_point){20, 50};
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    uint32_t b_state = optics_ui_get_response(ui).state;
    optics_ui_test_end(ui);
    OT_EXPECT(b_state & OPTICS_UI_STATE_FOCUSED);
    OT_EXPECT(!(b_state & OPTICS_UI_STATE_FOCUS_VISIBLE));
    OT_EXPECT(b_state & OPTICS_UI_STATE_PRESSED);

    optics_ui_release(ui);
}

static void test_widget_owned_bits(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool on = true;
    optics_ui_begin(ui, &IN0);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Sel", .selected = true});
    uint32_t sel = optics_ui_get_response(ui).state;
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Unsel", .selected = false});
    uint32_t unsel = optics_ui_get_response(ui).state;
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "On", .value = &on});
    uint32_t toggle = optics_ui_get_response(ui).state;
    optics_ui_test_end(ui);

    OT_EXPECT(sel & OPTICS_UI_STATE_SELECTED);
    OT_EXPECT(!(unsel & OPTICS_UI_STATE_SELECTED));
    OT_EXPECT(toggle & OPTICS_UI_STATE_ACTIVE);

    optics_ui_release(ui);
}

/* ---- resolver: fallback, override, derivation, dim order ------------ */

static void test_resolver_null_instance_is_verbatim_theme(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme t = optics_ui_theme_dark();
    optics_ui_style_resolved r =
        optics_ui__style_resolve(ui, nullptr, &t,
                                 OPTICS_UI_STATE_HOVERED | OPTICS_UI_STATE_PRESSED |
                                     OPTICS_UI_STATE_FOCUSED | OPTICS_UI_STATE_DISABLED);
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

    optics_ui_release(ui);
}

static void test_resolver_override_and_derivation(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme t = optics_ui_theme_dark();
    optics_gfx_color red = optics_gfx_color_rgba(0xc0, 0x20, 0x20, 0xff);

    optics_ui_style inst = optics_ui_style_init();
    inst.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_PADDING;
    inst.bg = red;
    inst.padding = 3.0f;

    optics_ui_style_resolved r = optics_ui__style_resolve(ui, &inst, &t, OPTICS_UI_STATE_HOVERED);
    OT_EXPECT(r.bg == red);
    OT_EXPECT(r.fg == t.color_fg);
    OT_EXPECT(r.accent == t.color_accent);
    OT_EXPECT_NEAR(r.padding, 3.0f, 0.0f);
    OT_EXPECT_NEAR(r.gap, t.gap, 0.0f);
    OT_EXPECT(r.bg_hover == optics_ui__lerp_color(red, t.color_fg, OPTICS_UI_STYLE_HOVER_LIFT));
    OT_EXPECT(r.bg_pressed ==
              optics_ui__lerp_color(red, t.color_fg, OPTICS_UI_STYLE_PRESSED_DEPTH));
    OT_EXPECT(r.bg_hover != t.color_hover);

    optics_gfx_color green = optics_gfx_color_rgba(0x20, 0xc0, 0x20, 0xff);
    inst.fields |= OPTICS_UI_STYLE_BG_HOVER;
    inst.bg_hover = green;
    r = optics_ui__style_resolve(ui, &inst, &t, OPTICS_UI_STATE_HOVERED);
    OT_EXPECT(r.bg_hover == green);
    OT_EXPECT(r.bg_pressed ==
              optics_ui__lerp_color(red, t.color_fg, OPTICS_UI_STYLE_PRESSED_DEPTH));

    optics_ui_release(ui);
}

static void test_resolver_disabled_dims_last(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme t = optics_ui_theme_dark();
    optics_gfx_color red = optics_gfx_color_rgba(0xc0, 0x20, 0x20, 0xff);

    optics_ui_style inst = optics_ui_style_init();
    inst.fields = OPTICS_UI_STYLE_BG;
    inst.bg = red;

    optics_ui_style_resolved en = optics_ui__style_resolve(ui, &inst, &t, 0);
    optics_ui_style_resolved dis = optics_ui__style_resolve(
        ui, &inst, &t,
        OPTICS_UI_STATE_DISABLED | OPTICS_UI_STATE_HOVERED | OPTICS_UI_STATE_PRESSED);

    OT_EXPECT(dis.bg ==
              optics_ui__lerp_color(en.bg, t.color_disabled, OPTICS_UI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.bg_hover ==
              optics_ui__lerp_color(en.bg_hover, t.color_disabled, OPTICS_UI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.bg_pressed ==
              optics_ui__lerp_color(en.bg_pressed, t.color_disabled, OPTICS_UI_STYLE_DISABLED_DIM));
    OT_EXPECT(dis.fg == t.color_fg);
    OT_EXPECT(dis.accent == t.color_accent);

    optics_ui_style_resolved a = optics_ui__style_resolve(ui, &inst, &t, OPTICS_UI_STATE_HOVERED);
    optics_ui_style_resolved b = optics_ui__style_resolve(ui, &inst, &t, OPTICS_UI_STATE_HOVERED);
    OT_EXPECT(memcmp(&a, &b, sizeof a) == 0);
    OT_EXPECT(inst.fields == OPTICS_UI_STYLE_BG);
    OT_EXPECT(inst.bg == red);

    optics_ui_release(ui);
}

static bool has_border_cmd(const optics_ui_node *n) {
    if (!n)
        return false;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == OPTICS_UI_DRAW_BORDER)
            return true;
    }
    return false;
}

static void test_minimalist_theme_defaults_and_borders(void) {
    optics_ui_theme def = optics_ui_theme_default();
    optics_ui_theme dark = optics_ui_theme_dark();
    optics_ui_theme classic_light = optics_ui_theme_classic(false);
    optics_ui_theme classic_dark = optics_ui_theme_classic(true);
    optics_ui_theme mat = optics_ui_theme_for_material(OPTICS_UI_MATERIAL_LIQUID_GLASS, true);

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

    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = {.display_size = {800, 600}, .dt_seconds = 0.016f};

    /* Frame 1: default theme (minimalist, border_width = 0) */
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn1"});
    bool chk = false;
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "chk1", .value = &chk});
    char text_buf[32] = "hello";
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    optics_ui_test_end(ui);

    optics_ui_node *n_btn = optics_ui_find(ui, optics_ui_current_id(ui, "btn1"));
    optics_ui_node *n_chk = optics_ui_find(ui, optics_ui_current_id(ui, "chk1"));
    optics_ui_node *n_tf = optics_ui_find(ui, optics_ui_current_id(ui, "tf1"));

    OT_EXPECT(n_btn != nullptr);
    OT_EXPECT(!has_border_cmd(n_btn));
    OT_EXPECT(n_chk != nullptr);
    OT_EXPECT(!has_border_cmd(n_chk));
    OT_EXPECT(n_tf != nullptr);
    OT_EXPECT(!has_border_cmd(n_tf));

    /* Frame 2: classic theme (border_width = 1.0) */
    optics_ui_set_theme(ui, classic_dark);
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn1"});
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "chk1", .value = &chk});
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    optics_ui_test_end(ui);

    n_btn = optics_ui_find(ui, optics_ui_current_id(ui, "btn1"));
    n_chk = optics_ui_find(ui, optics_ui_current_id(ui, "chk1"));
    n_tf = optics_ui_find(ui, optics_ui_current_id(ui, "tf1"));

    OT_EXPECT(n_btn != nullptr && has_border_cmd(n_btn));
    OT_EXPECT(n_chk != nullptr && has_border_cmd(n_chk));
    OT_EXPECT(n_tf != nullptr && has_border_cmd(n_tf));

    /* Frame 3: default theme with focused textedit -> emits focus outline */
    optics_ui_set_theme(ui, dark);
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "tf1"));
    optics_ui_begin(ui, &in);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "tf1"}, .buf = text_buf, .cap = sizeof text_buf});
    optics_ui_test_end(ui);

    n_tf = optics_ui_find(ui, optics_ui_current_id(ui, "tf1"));
    OT_EXPECT(n_tf != nullptr && has_border_cmd(n_tf));

    optics_ui_release(ui);
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
