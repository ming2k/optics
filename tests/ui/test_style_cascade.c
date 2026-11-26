/* test_style_cascade.c — the ADR-0061 cascade: per-call box.style over the
 * nearest enclosing scope over the theme, per field.
 *
 * Covers:
 *   - the scope stack: terse widgets pick scope atoms up, nested scopes win
 *     per field (nearest enclosing), a forgotten pop cannot leak into the
 *     next frame.
 *   - precedence: box.style beats the scope, the scope beats the theme.
 *   - the outline atoms reaching a widget's draw commands.
 *   - optics_ui_skin_scratch: zeroed on first touch, persistent across frames,
 *     reclaimed with the node by the store GC.
 *
 * style.c is compiled into the test binary directly (same pattern as
 * test_state_style): the cascade helpers are hidden in liboptics_ui. internal.h
 * is included for the node draw-list walk.
 */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static optics_ui_node *find_widget(optics_ui *ui, const char *label) {
    return optics_ui_find(ui, optics_ui_current_id(ui, label));
}

/* First text command colour on a node, or 0. */
static optics_gfx_color first_text_color(const optics_ui_node *n) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == OPTICS_UI_DRAW_TEXT)
            return n->cmds[i].color;
    return 0;
}

static optics_ui_style fg_only(optics_gfx_color c) {
    optics_ui_style s = optics_ui_style_init();
    s.fields = OPTICS_UI_STYLE_FG;
    s.fg = c;
    return s;
}

/* ---- merge: the per-field overlay primitive -------------------------- */

static void test_merge_is_per_field(void) {
    optics_ui_style base = fg_only(0xFF112233u);
    base.fields |= OPTICS_UI_STYLE_PADDING;
    base.padding = 9.0f;
    optics_ui_style over = optics_ui_style_init();
    over.fields = OPTICS_UI_STYLE_FG | OPTICS_UI_STYLE_FONT_SIZE;
    over.fg = 0xFF445566u;
    over.font_size = 22.0f;

    optics_ui_style m = optics_ui__style_merge(&base, &over);
    OT_EXPECT(m.fg == 0xFF445566u); /* over wins where set        */
    OT_EXPECT_NEAR(m.font_size, 22.0f, 0.0f);
    OT_EXPECT_NEAR(m.padding, 9.0f, 0.0f); /* base fills the rest        */
    OT_EXPECT(m.fields ==
              (OPTICS_UI_STYLE_FG | OPTICS_UI_STYLE_PADDING | OPTICS_UI_STYLE_FONT_SIZE));
    OT_EXPECT_NEAR(base.font_size, 0.0f, 0.0f); /* inputs untouched (pure) */
}

/* ---- the scope stack ------------------------------------------------- */

static void test_scope_reaches_terse_widgets(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_gfx_color red = optics_gfx_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "before"});
    optics_ui_push_style(ui, fg_only(red));
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "scoped"});
    optics_ui_pop_style(ui);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "after"});
    optics_ui_test_end(ui);

    optics_gfx_color themed = optics_ui_get_theme(ui).color_fg;
    OT_EXPECT(first_text_color(find_widget(ui, "before")) == themed);
    OT_EXPECT(first_text_color(find_widget(ui, "scoped")) == red);
    OT_EXPECT(first_text_color(find_widget(ui, "after")) == themed);
    optics_ui_release(ui);
}

static void test_nested_scopes_merge_and_nearest_wins(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_gfx_color red = optics_gfx_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);
    optics_gfx_color green = optics_gfx_color_rgba_premul(0x20, 0xC0, 0x20, 0xFF);

    optics_ui_style outer = fg_only(red);
    outer.fields |= OPTICS_UI_STYLE_FONT_SIZE;
    outer.font_size = 21.0f;
    optics_ui_style inner = fg_only(green); /* only fg */

    optics_ui_begin(ui, &IN0);
    optics_ui_push_style(ui, outer);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "outer"});
    optics_ui_push_style(ui, inner);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "inner"});
    optics_ui_pop_style(ui);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "back-to-outer"});
    optics_ui_pop_style(ui);
    optics_ui_test_end(ui);

    /* The inner scope overrides fg but inherits the outer font size. */
    optics_ui_node *in_node = find_widget(ui, "inner");
    OT_EXPECT(first_text_color(in_node) == green);
    OT_EXPECT(in_node && in_node->cmds[0].text_size == 21.0f);
    OT_EXPECT(first_text_color(find_widget(ui, "outer")) == red);
    optics_ui_node *back = find_widget(ui, "back-to-outer");
    OT_EXPECT(first_text_color(back) == red); /* pop restored the outer scope */
    OT_EXPECT(back && back->cmds[0].text_size == 21.0f);
    optics_ui_release(ui);
}

static void test_forgotten_pop_cannot_leak_across_frames(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_gfx_color red = optics_gfx_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);

    optics_ui_begin(ui, &IN0);
    optics_ui_push_style(ui, fg_only(red));
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "scoped"});
    /* no pop — the frame ends with the stack non-empty */
    optics_ui_test_end(ui);
    OT_EXPECT(first_text_color(find_widget(ui, "scoped")) == red);

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "next-frame"});
    optics_ui_test_end(ui);
    OT_EXPECT(first_text_color(find_widget(ui, "next-frame")) == optics_ui_get_theme(ui).color_fg);
    optics_ui_release(ui);
}

/* ---- precedence: per-call > scope > theme ---------------------------- */

static optics_ui_style bg_only(optics_gfx_color c) {
    optics_ui_style s = optics_ui_style_init();
    s.fields = OPTICS_UI_STYLE_BG;
    s.bg = c;
    return s;
}

/* The button body rect colour at rest (no hover/press): the skin paints
 * the resolved bg verbatim when OPTICS_UI_STYLE_BG came from the cascade. */
static optics_gfx_color first_rect_color(const optics_ui_node *n) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == OPTICS_UI_DRAW_RECT)
            return n->cmds[i].color;
    return 0;
}

static void test_box_style_beats_scope_beats_theme(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_gfx_color red = optics_gfx_color_rgba_premul(0xC0, 0x20, 0x20, 0xFF);
    optics_gfx_color blue = optics_gfx_color_rgba_premul(0x20, 0x20, 0xC0, 0xFF);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "themed"});
    optics_ui_push_style(ui, bg_only(red));
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "scoped"});
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "per-call", .box = {.style = bg_only(blue)}});
    optics_ui_pop_style(ui);
    optics_ui_test_end(ui);

    optics_gfx_color themed = optics_ui_get_theme(ui).color_bg;
    OT_EXPECT(first_rect_color(find_widget(ui, "themed")) == themed);
    OT_EXPECT(first_rect_color(find_widget(ui, "scoped")) == red);
    OT_EXPECT(first_rect_color(find_widget(ui, "per-call")) == blue);
    optics_ui_release(ui);
}

/* ---- outline atoms ---------------------------------------------------- */

static void test_outline_atoms_reach_draw_commands(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_gfx_color contour = optics_gfx_color_rgba_premul(0, 0, 0, 180);

    optics_ui_style s = optics_ui_style_init();
    s.fields = OPTICS_UI_STYLE_OUTLINE_COLOR | OPTICS_UI_STYLE_OUTLINE_WIDTH;
    s.outline_color = contour;
    s.outline_width = 0.75f;

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "plain"});
    optics_ui_push_style(ui, s);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "12:34", .size = 14.0f});
    optics_ui_pop_style(ui);
    optics_ui_test_end(ui);

    optics_ui_node *plain = find_widget(ui, "plain");
    optics_ui_node *outlined = find_widget(ui, "12:34");
    OT_EXPECT(plain != nullptr && outlined != nullptr);
    if (plain && plain->cmd_count > 0) {
        OT_EXPECT(plain->cmds[0].outline_width == 0.0f); /* unset = no contour */
        OT_EXPECT(plain->cmds[0].outline_color == 0);
    }
    if (outlined && outlined->cmd_count > 0) {
        OT_EXPECT(outlined->cmds[0].outline_color == contour);
        OT_EXPECT_NEAR(outlined->cmds[0].outline_width, 0.75f, 0.0f);
    }
    optics_ui_release(ui);
}

/* ---- per-node scratch (ADR-0061 item 9) ------------------------------- */

static void test_skin_scratch_lifecycle(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Host"});
    optics_ui_test_end(ui);

    optics_ui_node *n = find_widget(ui, "Host");
    OT_EXPECT(n != nullptr);
    float *scratch = optics_ui_skin_scratch(ui, n);
    OT_EXPECT(scratch != nullptr);
    /* Zeroed on first touch. */
    for (int i = 0; i < 4; i++)
        OT_EXPECT_NEAR(scratch[i], 0.0f, 0.0f);

    /* Persistent across frames: write once, read back next frame. */
    scratch[0] = 12.5f;
    scratch[3] = -4.25f;
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Host"});
    optics_ui_test_end(ui);
    n = find_widget(ui, "Host");
    OT_EXPECT(n != nullptr);
    scratch = optics_ui_skin_scratch(ui, n);
    OT_EXPECT(scratch != nullptr);
    OT_EXPECT_NEAR(scratch[0], 12.5f, 0.0f);
    OT_EXPECT_NEAR(scratch[3], -4.25f, 0.0f);

    /* Reclaimed with the node: stop declaring the widget past the reap
     * grace (OPTICS_UI_LEAVE_GRACE_FRAMES) and the slot — scratch included —
     * is gone. */
    for (int f = 0; f < 12; f++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "unrelated"});
        optics_ui_test_end(ui);
    }
    OT_EXPECT(find_widget(ui, "Host") == nullptr);
    OT_EXPECT(optics_ui_skin_scratch(ui, nullptr) == nullptr); /* NULL-safety */

    optics_ui_release(ui);
}

int main(void) {
    test_merge_is_per_field();
    test_scope_reaches_terse_widgets();
    test_nested_scopes_merge_and_nearest_wins();
    test_forgotten_pop_cannot_leak_across_frames();
    test_box_style_beats_scope_beats_theme();
    test_outline_atoms_reach_draw_commands();
    test_skin_scratch_lifecycle();
    return OT_REPORT();
}
