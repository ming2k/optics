/* test_state_style.c — widget state bits and per-instance styles (ADR-0058,
 * cascade amendments ADR-0061).
 *
 * Covers:
 *   - state bit production through synthetic frames: hover sets HOVERED,
 *     press sets PRESSED|HOVERED, disabled sets DISABLED (and nothing
 *     else), Tab focus sets FOCUSED|FOCUS_VISIBLE while pointer focus sets
 *     FOCUSED alone, selectable/checkbox contribute SELECTED/ACTIVE.
 *   - the style resolver: NULL instance falls back to the theme verbatim
 *     for every state, set fields override, hover/pressed derivations come
 *     off a cascaded base, and the disabled dim applies after them.
 *   - the accent rail width coming from a per-call box.style, not the theme.
 *   - pixel identity: an empty-styled widget emits the same draw commands
 *     as the plain form.
 * The cascade itself (scope stack, per-field merge, precedence) lives in
 * test_style_cascade.c.
 *
 * style.c is compiled into the test binary directly (same pattern as
 * test_drawlist_hash): lensi_style_resolve is hidden in liblens, so the
 * test links its own copy. internal.h is included for the resolved struct,
 * the derivation constants, and the node draw-list walk.
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
    lens_button(ui, "OK");
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    lens_begin(ui, &in);
    lens_button(ui, "OK");
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
    lens_button(ui, "OK");
    uint32_t press_state = lens_get_response(ui).state;
    lens_end(ui);
    CHECK((press_state & LENS_STATE_PRESSED) && (press_state & LENS_STATE_HOVERED));

    lens_destroy(ui);
}

static void test_disabled_bit_excludes_hover_and_press(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button_ex(ui, (lens_button_opts){.label = "OK", .box = {.disabled = true}});
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
    lens_button(ui, "A");
    lens_button(ui, "B");
    lens_end(ui);

    /* Frame 2: Tab moves focus to A during lens_end. */
    lens_input in = IN0;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_TAB, .pressed = true, .repeat = false};
    in.key_count = 1;
    lens_begin(ui, &in);
    lens_button(ui, "A");
    lens_button(ui, "B");
    lens_end(ui);

    /* Frame 3: A reports FOCUSED|FOCUS_VISIBLE (keyboard modality). */
    lens_begin(ui, &IN0);
    lens_button(ui, "A");
    uint32_t a_state = lens_get_response(ui).state;
    lens_button(ui, "B");
    lens_end(ui);
    CHECK(a_state & LENS_STATE_FOCUSED);
    CHECK(a_state & LENS_STATE_FOCUS_VISIBLE);

    /* Frame 4: a pointer press on B focuses it without FOCUS_VISIBLE.
     * A is 14 + 2*12 = 38 logical px tall, so (20, 50) lands on B. */
    in = IN0;
    in.cursor = (flux_point){20, 50};
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, "A");
    lens_button(ui, "B");
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
    lens_selectable(ui, "Sel", true);
    uint32_t sel = lens_get_response(ui).state;
    lens_selectable(ui, "Unsel", false);
    uint32_t unsel = lens_get_response(ui).state;
    lens_checkbox(ui, "On", &on);
    uint32_t toggle = lens_get_response(ui).state;
    lens_end(ui);

    CHECK(sel & LENS_STATE_SELECTED);
    CHECK(!(unsel & LENS_STATE_SELECTED));
    CHECK(toggle & LENS_STATE_ACTIVE);

    lens_destroy(ui);
}

/* ---- resolver: fallback, override, derivation, dim order ------------ */

static void test_resolver_null_instance_is_verbatim_theme(void) {
    lens_theme t = lens_theme_dark();
    /* Even with every interactive state bit set, the theme path must come
     * through untouched — this is what keeps migrated widgets
     * pixel-identical. */
    lens_style_resolved r =
        lensi_style_resolve(NULL, &t, LENS_STATE_HOVERED | LENS_STATE_PRESSED |
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
}

static void test_resolver_override_and_derivation(void) {
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG | LENS_STYLE_PADDING;
    inst.bg = red;
    inst.padding = 3.0f;

    lens_style_resolved r = lensi_style_resolve(&inst, &t, LENS_STATE_HOVERED);
    CHECK(r.bg == red);                    /* set field wins           */
    CHECK(r.fg == t.color_fg);             /* unset field falls back   */
    CHECK(r.accent == t.color_accent);
    CHECK_NEAR(r.padding, 3.0f, 0.0f);
    CHECK_NEAR(r.gap, t.gap, 0.0f);
    /* Unset state slots derive off the instance base: hover lift, then
     * pressed deepen (both toward the resolved foreground). */
    CHECK(r.bg_hover == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_HOVER_LIFT));
    CHECK(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));
    CHECK(r.bg_hover != t.color_hover);

    /* An explicit state slot beats the derivation. */
    flux_color green = flux_color_rgba(0x20, 0xc0, 0x20, 0xff);
    inst.fields |= LENS_STYLE_BG_HOVER;
    inst.bg_hover = green;
    r = lensi_style_resolve(&inst, &t, LENS_STATE_HOVERED);
    CHECK(r.bg_hover == green);
    CHECK(r.bg_pressed == lensi_lerp_color(red, t.color_fg, LENSI_STYLE_PRESSED_DEPTH));
}

static void test_resolver_disabled_dims_last(void) {
    lens_theme t = lens_theme_dark();
    flux_color red = flux_color_rgba(0xc0, 0x20, 0x20, 0xff);

    lens_style inst = lens_style_init();
    inst.fields = LENS_STYLE_BG;
    inst.bg = red;

    lens_style_resolved en = lensi_style_resolve(&inst, &t, 0);
    lens_style_resolved dis = lensi_style_resolve(
        &inst, &t, LENS_STATE_DISABLED | LENS_STATE_HOVERED | LENS_STATE_PRESSED);

    /* The dim applies on top of the hover/pressed adjustments: the derived
     * hover/pressed values are what get dimmed, not the raw base. */
    CHECK(dis.bg == lensi_lerp_color(en.bg, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    CHECK(dis.bg_hover ==
          lensi_lerp_color(en.bg_hover, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    CHECK(dis.bg_pressed ==
          lensi_lerp_color(en.bg_pressed, t.color_disabled, LENSI_STYLE_DISABLED_DIM));
    /* Theme-sourced slots are not dimmed: the theme already carries a
     * designed disabled token. */
    CHECK(dis.fg == t.color_fg);
    CHECK(dis.accent == t.color_accent);

    /* Purity: same inputs, same output; the instance style is untouched. */
    lens_style_resolved a = lensi_style_resolve(&inst, &t, LENS_STATE_HOVERED);
    lens_style_resolved b = lensi_style_resolve(&inst, &t, LENS_STATE_HOVERED);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    CHECK(inst.fields == LENS_STYLE_BG);
    CHECK(inst.bg == red);
}

/* ---- NULL-style pixel identity ---------------------------------------- */

/* Compare two command streams field by field (skipping the arena text
 * pointer, which legitimately differs between contexts). */
static bool cmds_equal(const lens_node *a, const lens_node *b) {
    if (!a || !b || a->cmd_count != b->cmd_count)
        return false;
    for (uint32_t i = 0; i < a->cmd_count; i++) {
        const lens_draw_cmd *x = &a->cmds[i];
        const lens_draw_cmd *y = &b->cmds[i];
        if (x->kind != y->kind || x->color != y->color || x->radius != y->radius ||
            x->width != y->width || x->text_size != y->text_size ||
            memcmp(&x->rel, &y->rel, sizeof x->rel) != 0)
            return false;
    }
    return true;
}

static void test_null_style_matches_plain_form(void) {
    /* Two contexts, identical frames incl. a hover so the eased hover_t is
     * non-trivial: plain form vs the descriptor form with an empty
     * box.style must emit identical cmds. */
    lens *a = NULL;
    lens *b = NULL;
    CHECK(lens_create(&(lens_desc){0}, &a) == FLUX_OK);
    CHECK(lens_create(&(lens_desc){0}, &b) == FLUX_OK);

    for (int frame = 0; frame < 2; frame++) {
        lens_input in = IN0;
        if (frame == 1)
            in.cursor = (flux_point){20, 20};
        lens_begin(a, &in);
        lens_button(a, "OK");
        lens_selectable(a, "Row", true);
        lens_end(a);
        lens_begin(b, &in);
        lens_button_ex(b, (lens_button_opts){.label = "OK"});
        lens_selectable_ex(b, (lens_selectable_opts){.label = "Row", .selected = true});
        lens_end(b);
    }

    lens_node *na = lens_find(a, lens_current_id(a, "OK"));
    lens_node *nb = lens_find(b, lens_current_id(b, "OK"));
    CHECK(cmds_equal(na, nb));
    na = lens_find(a, lens_current_id(a, "Row"));
    nb = lens_find(b, lens_current_id(b, "Row"));
    CHECK(cmds_equal(na, nb));

    lens_destroy(a);
    lens_destroy(b);
}

int main(void) {
    test_hover_and_press_bits();
    test_disabled_bit_excludes_hover_and_press();
    test_focus_visible_is_keyboard_only();
    test_widget_owned_bits();
    test_resolver_null_instance_is_verbatim_theme();
    test_resolver_override_and_derivation();
    test_resolver_disabled_dims_last();
    test_null_style_matches_plain_form();
    return TEST_REPORT();
}
