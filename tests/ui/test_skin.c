/* test_skin.c — the skin layer (ADR-0059).
 * Tests dispatch and replacement for all 11 orthogonal widget kinds. */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static bool cmds_equal(const optics_ui_node *a, const optics_ui_node *b) {
    if (!a || !b || a->cmd_count != b->cmd_count)
        return false;
    for (uint32_t i = 0; i < a->cmd_count; i++) {
        const optics_ui_draw_cmd *x = &a->cmds[i];
        const optics_ui_draw_cmd *y = &b->cmds[i];
        if (x->kind != y->kind || x->color != y->color ||
            x->corner_radii.tl != y->corner_radii.tl || x->corner_radii.tr != y->corner_radii.tr ||
            x->corner_radii.br != y->corner_radii.br || x->corner_radii.bl != y->corner_radii.bl ||
            x->corner_curvature != y->corner_curvature || x->width != y->width ||
            x->text_size != y->text_size || x->text_weight != y->text_weight ||
            x->icon_id != y->icon_id || memcmp(&x->rel, &y->rel, sizeof x->rel) != 0)
            return false;
    }
    return true;
}

static optics_ui_node *find_widget(optics_ui *ui, const char *label) {
    return optics_ui_find(ui, optics_ui_current_id(ui, label));
}

/* ---- null-override equality ----------------------------------------- */

typedef void (*build_fn)(optics_ui *ui, bool skinned_null);

static void check_null_override(const char *label, build_fn plain, build_fn skinned_null,
                                bool explicit_default, optics_ui_widget_kind kind) {
    optics_ui *a = nullptr;
    optics_ui *b = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &b) == OPTICS_GFX_OK);
    if (explicit_default)
        optics_ui_set_skin(b, kind, optics_ui_default_skin(kind));

    for (int frame = 0; frame < 2; frame++) {
        optics_ui_input in = IN0;
        if (frame == 1)
            in.cursor = (optics_gfx_point){20, 20};
        optics_ui_begin(a, &in);
        plain(a, false);
        optics_ui_test_end(a);
        optics_ui_begin(b, &in);
        skinned_null(b, true);
        optics_ui_test_end(b);
    }

    OT_EXPECT(cmds_equal(find_widget(a, label), find_widget(b, label)));
    optics_ui_release(a);
    optics_ui_release(b);
}

static void build_button_plain(optics_ui *ui, bool s) {
    (void)s;
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
}
static void build_selectable_plain(optics_ui *ui, bool s) {
    (void)s;
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Row", .selected = true});
}
static void build_checkbox_plain(optics_ui *ui, bool s) {
    (void)s;
    bool v = true;
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Check", .value = &v});
}
static void build_slider_plain(optics_ui *ui, bool s) {
    (void)s;
    float v = 25.0f;
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Sli", .value = &v, .min = 0.0f, .max = 100.0f});
}

static void test_null_override_equality(void) {
    check_null_override("OK", build_button_plain, build_button_plain, true,
                        OPTICS_UI_WIDGET_BUTTON);
    check_null_override("Row", build_selectable_plain, build_selectable_plain, true,
                        OPTICS_UI_WIDGET_SELECTABLE);
    check_null_override("Check", build_checkbox_plain, build_checkbox_plain, true,
                        OPTICS_UI_WIDGET_CHECKBOX);
    check_null_override("Sli", build_slider_plain, build_slider_plain, true,
                        OPTICS_UI_WIDGET_SLIDER);
}

/* ---- custom context skin --------------------------------------------- */

static optics_ui_widget_record g_seen;

static void underline_skin(optics_ui *ui, optics_ui_node *node,
                           const optics_ui_widget_record *rec) {
    g_seen = *rec;
    const optics_ui_style_resolved *rs = &rec->style;
    if (rec->state & OPTICS_UI_STATE_SELECTED)
        optics_ui_skin_rect(
            ui, &(optics_ui_skin_rect_opts){
                    .node = node,
                    .rel = (optics_gfx_rect){0, rec->bounds.h - 2.0f, rec->bounds.w, 2.0f},
                    .color = rs->accent,
                });
    optics_ui_skin_text(ui, &(optics_ui_skin_text_opts){
                                .node = node,
                                .rel = (optics_gfx_rect){rs->padding, 0, 0, -1.0f},
                                .color = rs->fg,
                                .utf8 = rec->content.label,
                                .size_px = rs->font_size,
                            });
}

static void test_custom_skin_record_and_output(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme t = optics_ui_get_theme(ui);

    optics_ui_begin(ui, &IN0);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Row", .selected = true});
    optics_ui_test_end(ui);
    optics_ui_node *def = find_widget(ui, "Row");
    OT_EXPECT(def != nullptr);
    uint32_t def_cmds = def ? def->cmd_count : 0;

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_SELECTABLE, underline_skin);
    optics_ui_begin(ui, &in);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Row", .selected = true});
    optics_ui_test_end(ui);
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_SELECTABLE, nullptr);

    OT_EXPECT(g_seen.kind == OPTICS_UI_WIDGET_SELECTABLE);
    OT_EXPECT(g_seen.state & OPTICS_UI_STATE_SELECTED);
    OT_EXPECT(g_seen.state & OPTICS_UI_STATE_HOVERED);
    OT_EXPECT(g_seen.bounds.w > 0.0f && g_seen.bounds.h > 0.0f);
    OT_EXPECT(g_seen.last_bounds.w > 0.0f);
    OT_EXPECT(g_seen.style.accent == t.color_accent);
    OT_EXPECT(g_seen.style_fields == 0);
    OT_EXPECT(g_seen.content.label != nullptr);
    OT_EXPECT(g_seen.hover_t == 1.0f);

    optics_ui_node *n = find_widget(ui, "Row");
    OT_EXPECT(n != nullptr);
    OT_EXPECT(n && n->cmd_count == 2);
    OT_EXPECT(def_cmds >= 2);
    if (n && n->cmd_count == 2) {
        OT_EXPECT(n->cmds[0].kind == OPTICS_UI_DRAW_RECT);
        OT_EXPECT(n->cmds[0].color == t.color_accent);
        OT_EXPECT_NEAR(n->cmds[0].rel.h, 2.0f, 0.0f);
        OT_EXPECT(n->cmds[1].kind == OPTICS_UI_DRAW_TEXT);
    }

    optics_ui_release(ui);
}

/* ---- context-wide override ------------------------------------------- */

static bool g_context_skin_ran;

static void flat_button_skin(optics_ui *ui, optics_ui_node *node,
                             const optics_ui_widget_record *rec) {
    g_context_skin_ran = true;
    optics_ui_skin_rect(ui,
                        &(optics_ui_skin_rect_opts){.node = node, .color = rec->style.bg_pressed});
}

static void hollow_button_skin(optics_ui *ui, optics_ui_node *node,
                               const optics_ui_widget_record *rec) {
    optics_ui_skin_border(
        ui, &(optics_ui_skin_border_opts){.node = node, .color = rec->style.fg, .width = 1.0f});
}

static void test_context_skin_override(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);
    optics_ui_node *n = find_widget(ui, "OK");
    OT_EXPECT(n != nullptr);
    uint32_t default_cmds = n ? n->cmd_count : 0;
    OT_EXPECT(default_cmds >= 2);

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_BUTTON, flat_button_skin);
    g_context_skin_ran = false;
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);
    OT_EXPECT(g_context_skin_ran);
    OT_EXPECT(n && n->cmd_count == 1);
    OT_EXPECT(n && n->cmds[0].kind == OPTICS_UI_DRAW_RECT);

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_BUTTON, hollow_button_skin);
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);
    OT_EXPECT(n && n->cmd_count == 1);
    OT_EXPECT(n && n->cmds[0].kind == OPTICS_UI_DRAW_BORDER);

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_BUTTON, nullptr);
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);
    OT_EXPECT(n && n->cmd_count == default_cmds);
    OT_EXPECT(n && n->cmds[0].kind == OPTICS_UI_DRAW_RECT);

    optics_ui_release(ui);
}

/* ---- every kind dispatches and is replaceable ------------------------ */

static optics_ui_widget_record g_probe_seen;
static bool g_probe_ran;

static void probe_skin(optics_ui *ui, optics_ui_node *node, const optics_ui_widget_record *rec) {
    g_probe_ran = true;
    g_probe_seen = *rec;
    optics_ui_skin_border(
        ui, &(optics_ui_skin_border_opts){.node = node, .color = rec->style.accent, .width = 1.0f});
}

typedef void (*probe_build_fn)(optics_ui *ui);
static const char *g_find_name;

static optics_ui_node *find_named(optics_ui *ui) {
    return find_widget(ui, g_find_name);
}
static optics_ui_node *find_sep(optics_ui *ui) {
    return find_widget(ui, "##sep");
}
static optics_ui_node *find_first_child(optics_ui *ui) {
    return optics_ui_node_first_child(optics_ui_root(ui));
}

static void build_label(optics_ui *ui) {
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Lbl"});
}
static void build_separator(optics_ui *ui) {
    optics_ui_separator(ui, nullptr);
}
static void build_icon(optics_ui *ui) {
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_CHEVRON_RIGHT, .size = 24.0f});
}
static void build_image(optics_ui *ui) {
    optics_ui_image(ui, &(optics_ui_image_opts){.width = 32.0f, .height = 32.0f});
}
static void build_textedit(optics_ui *ui) {
    static char buf[64] = "abc";
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "Fld"}, .buf = buf, .cap = sizeof buf});
}

static void check_kind(const char *name, optics_ui_widget_kind kind, probe_build_fn build,
                       optics_ui_node *(*find)(optics_ui *ui)) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    build(ui);
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &IN0);
    build(ui);
    optics_ui_test_end(ui);
    optics_ui_node *n = find(ui);
    OT_EXPECT(n != nullptr);
    uint32_t default_cmds = n ? n->cmd_count : 0;

    optics_ui_set_skin(ui, kind, probe_skin);
    g_probe_ran = false;
    optics_ui_begin(ui, &IN0);
    build(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(g_probe_ran);
    OT_EXPECT(g_probe_seen.kind == kind);
    OT_EXPECT(n && n->cmd_count == 1);
    OT_EXPECT(n && n->cmds[0].kind == OPTICS_UI_DRAW_BORDER);

    optics_ui_set_skin(ui, kind, nullptr);
    optics_ui_begin(ui, &IN0);
    build(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(n && n->cmd_count == default_cmds);
    optics_ui_release(ui);
}

static void test_every_kind_dispatches_and_is_replaceable(void) {
    const struct {
        const char *name;
        optics_ui_widget_kind kind;
        probe_build_fn build;
        optics_ui_node *(*find)(optics_ui *ui);
    } cases[] = {
        {"label", OPTICS_UI_WIDGET_LABEL, build_label, find_named},
        {"separator", OPTICS_UI_WIDGET_SEPARATOR, build_separator, find_sep},
        {"icon", OPTICS_UI_WIDGET_ICON, build_icon, find_first_child},
        {"image", OPTICS_UI_WIDGET_IMAGE, build_image, find_first_child},
        {"textedit", OPTICS_UI_WIDGET_TEXTEDIT, build_textedit, find_named},
    };
    const char *names[] = {"Lbl", nullptr, nullptr, nullptr, "Fld"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_find_name = names[i];
        check_kind(cases[i].name, cases[i].kind, cases[i].build, cases[i].find);
    }
}

/* Selection highlight tint derived from `rs->accent` via `optics_ui__color_alpha`
 * must preserve the premultiplied sRGB invariant (R, G, B <= A). Replacing only
 * the alpha byte produced R,G,B > A, which rendered as opaque-looking grey. */
static void test_textedit_selection_color_is_premultiplied(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_theme theme = optics_ui_get_theme(ui);
    theme.color_accent = optics_gfx_color_rgba_premul(102, 156, 255, 255);
    optics_ui_set_theme(ui, theme);

    char buf[32] = "/home/user";
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "sel_tf", .width = 160.0f, .height = 32.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_textedit_set_selection(ui, &(optics_ui_textedit_set_selection_opts){
                                             .label = "sel_tf", .sel_start = 0, .sel_end = 10});
    optics_ui_test_end(ui);

    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "sel_tf", .width = 160.0f, .height = 32.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, "sel_tf"));
    OT_EXPECT(n != nullptr && n->cmd_count >= 2);
    if (n && n->cmd_count >= 2) {
        /* cmds[0] = background rect, cmds[1] = selection highlight rect */
        const optics_gfx_color col = n->cmds[1].color;
        const uint32_t a = (col >> 24) & 0xFFu;
        const uint32_t r = (col >> 16) & 0xFFu;
        const uint32_t g = (col >> 8) & 0xFFu;
        const uint32_t b = col & 0xFFu;
        OT_EXPECT(a == 0x40u);
        OT_EXPECT(r <= a && g <= a && b <= a);
        OT_EXPECT(b > g && g > r);
    }

    optics_ui_release(ui);
}

int main(void) {
    test_null_override_equality();
    test_custom_skin_record_and_output();
    test_context_skin_override();
    test_every_kind_dispatches_and_is_replaceable();
    test_textedit_selection_color_is_premultiplied();
    return OT_REPORT();
}
