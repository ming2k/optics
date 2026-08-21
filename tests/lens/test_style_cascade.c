/* test_style_cascade.c — the ADR-0061 cascade: per-call box.style over the
 * nearest enclosing scope over the theme, per field.
 *
 * Covers:
 *   - the scope stack: terse widgets pick scope atoms up, nested scopes win
 *     per field (nearest enclosing), a forgotten pop cannot leak into the
 *     next frame.
 *   - precedence: box.style beats the scope, the scope beats the theme.
 *   - the outline atoms reaching a widget's draw commands.
 *   - lens_skin_scratch: zeroed on first touch, persistent across frames,
 *     reclaimed with the node by the store GC.
 *
 * style.c is compiled into the test binary directly (same pattern as
 * test_state_style): the cascade helpers are hidden in liblens. internal.h
 * is included for the node draw-list walk.
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static lens_node *find_widget(lens *ui, const char *label) {
    return lens_find(ui, lens_current_id(ui, label));
}

/* First text command colour on a node, or 0. */
static flux_color first_text_color(const lens_node *n) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == LENS_DRAW_TEXT)
            return n->cmds[i].color;
    return 0;
}

static lens_style fg_only(flux_color c) {
    lens_style s = lens_style_init();
    s.fields = LENS_STYLE_FG;
    s.fg = c;
    return s;
}

/* ---- merge: the per-field overlay primitive -------------------------- */

static void test_merge_is_per_field(void) {
    lens_style base = fg_only(0xFF112233u);
    base.fields |= LENS_STYLE_PADDING;
    base.padding = 9.0f;
    lens_style over = lens_style_init();
    over.fields = LENS_STYLE_FG | LENS_STYLE_FONT_SIZE;
    over.fg = 0xFF445566u;
    over.font_size = 22.0f;

    lens_style m = lensi_style_merge(&base, &over);
    CHECK(m.fg == 0xFF445566u); /* over wins where set        */
    CHECK_NEAR(m.font_size, 22.0f, 0.0f);
    CHECK_NEAR(m.padding, 9.0f, 0.0f); /* base fills the rest        */
    CHECK(m.fields == (LENS_STYLE_FG | LENS_STYLE_PADDING | LENS_STYLE_FONT_SIZE));
    CHECK_NEAR(base.font_size, 0.0f, 0.0f); /* inputs untouched (pure) */
}

/* ---- the scope stack ------------------------------------------------- */

static void test_scope_reaches_terse_widgets(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    flux_color red = flux_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);

    lens_begin(ui, &IN0);
    lens_label(ui, "before");
    lens_push_style(ui, fg_only(red));
    lens_label(ui, "scoped");
    lens_pop_style(ui);
    lens_label(ui, "after");
    lens_end(ui);

    flux_color themed = lens_get_theme(ui).color_fg;
    CHECK(first_text_color(find_widget(ui, "before")) == themed);
    CHECK(first_text_color(find_widget(ui, "scoped")) == red);
    CHECK(first_text_color(find_widget(ui, "after")) == themed);
    lens_destroy(ui);
}

static void test_nested_scopes_merge_and_nearest_wins(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    flux_color red = flux_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);
    flux_color green = flux_color_rgba_premul(0x20, 0xC0, 0x20, 0xFF);

    lens_style outer = fg_only(red);
    outer.fields |= LENS_STYLE_FONT_SIZE;
    outer.font_size = 21.0f;
    lens_style inner = fg_only(green); /* only fg */

    lens_begin(ui, &IN0);
    lens_push_style(ui, outer);
    lens_label(ui, "outer");
    lens_push_style(ui, inner);
    lens_label(ui, "inner");
    lens_pop_style(ui);
    lens_label(ui, "back-to-outer");
    lens_pop_style(ui);
    lens_end(ui);

    /* The inner scope overrides fg but inherits the outer font size. */
    lens_node *in_node = find_widget(ui, "inner");
    CHECK(first_text_color(in_node) == green);
    CHECK(in_node && in_node->cmds[0].text_size == 21.0f);
    CHECK(first_text_color(find_widget(ui, "outer")) == red);
    lens_node *back = find_widget(ui, "back-to-outer");
    CHECK(first_text_color(back) == red); /* pop restored the outer scope */
    CHECK(back && back->cmds[0].text_size == 21.0f);
    lens_destroy(ui);
}

static void test_forgotten_pop_cannot_leak_across_frames(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    flux_color red = flux_color_rgba_premul(0xFF, 0x20, 0x20, 0xFF);

    lens_begin(ui, &IN0);
    lens_push_style(ui, fg_only(red));
    lens_label(ui, "scoped");
    /* no pop — the frame ends with the stack non-empty */
    lens_end(ui);
    CHECK(first_text_color(find_widget(ui, "scoped")) == red);

    lens_begin(ui, &IN0);
    lens_label(ui, "next-frame");
    lens_end(ui);
    CHECK(first_text_color(find_widget(ui, "next-frame")) == lens_get_theme(ui).color_fg);
    lens_destroy(ui);
}

/* ---- precedence: per-call > scope > theme ---------------------------- */

static lens_style bg_only(flux_color c) {
    lens_style s = lens_style_init();
    s.fields = LENS_STYLE_BG;
    s.bg = c;
    return s;
}

/* The button body rect colour at rest (no hover/press): the skin paints
 * the resolved bg verbatim when LENS_STYLE_BG came from the cascade. */
static flux_color first_rect_color(const lens_node *n) {
    if (!n)
        return 0;
    for (uint32_t i = 0; i < n->cmd_count; i++)
        if (n->cmds[i].kind == LENS_DRAW_RECT)
            return n->cmds[i].color;
    return 0;
}

static void test_box_style_beats_scope_beats_theme(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    flux_color red = flux_color_rgba_premul(0xC0, 0x20, 0x20, 0xFF);
    flux_color blue = flux_color_rgba_premul(0x20, 0x20, 0xC0, 0xFF);

    lens_begin(ui, &IN0);
    lens_button_ex(ui, (lens_button_opts){.label = "themed"});
    lens_push_style(ui, bg_only(red));
    lens_button_ex(ui, (lens_button_opts){.label = "scoped"});
    lens_button_ex(ui, (lens_button_opts){.label = "per-call", .box = {.style = bg_only(blue)}});
    lens_pop_style(ui);
    lens_end(ui);

    flux_color themed = lens_get_theme(ui).color_accent; /* default body: accent */
    CHECK(first_rect_color(find_widget(ui, "themed")) == themed);
    CHECK(first_rect_color(find_widget(ui, "scoped")) == red);
    CHECK(first_rect_color(find_widget(ui, "per-call")) == blue);
    lens_destroy(ui);
}

/* ---- outline atoms ---------------------------------------------------- */

static void test_outline_atoms_reach_draw_commands(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    flux_color contour = flux_color_rgba_premul(0, 0, 0, 180);

    lens_style s = lens_style_init();
    s.fields = LENS_STYLE_OUTLINE_COLOR | LENS_STYLE_OUTLINE_WIDTH;
    s.outline_color = contour;
    s.outline_width = 0.75f;

    lens_begin(ui, &IN0);
    lens_label(ui, "plain");
    lens_push_style(ui, s);
    lens_label_compact_ex(ui, "12:34", 14.0f);
    lens_pop_style(ui);
    lens_end(ui);

    lens_node *plain = find_widget(ui, "plain");
    lens_node *outlined = find_widget(ui, "12:34");
    CHECK(plain != NULL && outlined != NULL);
    if (plain && plain->cmd_count > 0) {
        CHECK(plain->cmds[0].outline_width == 0.0f); /* unset = no contour */
        CHECK(plain->cmds[0].outline_color == 0);
    }
    if (outlined && outlined->cmd_count > 0) {
        CHECK(outlined->cmds[0].outline_color == contour);
        CHECK_NEAR(outlined->cmds[0].outline_width, 0.75f, 0.0f);
    }
    lens_destroy(ui);
}

/* ---- per-node scratch (ADR-0061 item 9) ------------------------------- */

static void test_skin_scratch_lifecycle(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "Host");
    lens_end(ui);

    lens_node *n = find_widget(ui, "Host");
    CHECK(n != NULL);
    float *scratch = lens_skin_scratch(ui, n);
    CHECK(scratch != NULL);
    /* Zeroed on first touch. */
    for (int i = 0; i < 4; i++)
        CHECK_NEAR(scratch[i], 0.0f, 0.0f);

    /* Persistent across frames: write once, read back next frame. */
    scratch[0] = 12.5f;
    scratch[3] = -4.25f;
    lens_begin(ui, &IN0);
    lens_button(ui, "Host");
    lens_end(ui);
    n = find_widget(ui, "Host");
    CHECK(n != NULL);
    scratch = lens_skin_scratch(ui, n);
    CHECK(scratch != NULL);
    CHECK_NEAR(scratch[0], 12.5f, 0.0f);
    CHECK_NEAR(scratch[3], -4.25f, 0.0f);

    /* Reclaimed with the node: stop declaring the widget past the reap
     * grace (LENSI_LEAVE_GRACE_FRAMES) and the slot — scratch included —
     * is gone. */
    for (int f = 0; f < 12; f++) {
        lens_begin(ui, &IN0);
        lens_label(ui, "unrelated");
        lens_end(ui);
    }
    CHECK(find_widget(ui, "Host") == NULL);
    CHECK(lens_skin_scratch(ui, NULL) == NULL); /* NULL-safety */

    lens_destroy(ui);
}

int main(void) {
    test_merge_is_per_field();
    test_scope_reaches_terse_widgets();
    test_nested_scopes_merge_and_nearest_wins();
    test_forgotten_pop_cannot_leak_across_frames();
    test_box_style_beats_scope_beats_theme();
    test_outline_atoms_reach_draw_commands();
    test_skin_scratch_lifecycle();
    return TEST_REPORT();
}
