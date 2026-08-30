/* test_skin.c — the skin layer (ADR-0059).
 * Tests dispatch and replacement for all 11 orthogonal widget kinds. */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static bool cmds_equal(const lens_node *a, const lens_node *b) {
    if (!a || !b || a->cmd_count != b->cmd_count)
        return false;
    for (uint32_t i = 0; i < a->cmd_count; i++) {
        const lens_draw_cmd *x = &a->cmds[i];
        const lens_draw_cmd *y = &b->cmds[i];
        if (x->kind != y->kind || x->color != y->color || x->radius != y->radius ||
            x->width != y->width || x->text_size != y->text_size ||
            x->text_weight != y->text_weight || x->icon_id != y->icon_id ||
            memcmp(&x->rel, &y->rel, sizeof x->rel) != 0)
            return false;
    }
    return true;
}

static lens_node *find_widget(lens *ui, const char *label) {
    return lens_find(ui, lens_current_id(ui, label));
}

/* ---- null-override equality ----------------------------------------- */

typedef void (*build_fn)(lens *ui, bool skinned_null);

static void check_null_override(const char *label, build_fn plain, build_fn skinned_null,
                                bool explicit_default, lens_widget_kind kind) {
    lens *a = NULL;
    lens *b = NULL;
    CHECK(lens_create(&(lens_desc){0}, &a) == FLUX_OK);
    CHECK(lens_create(&(lens_desc){0}, &b) == FLUX_OK);
    if (explicit_default)
        lens_set_skin(b, kind, lens_default_skin(kind));

    for (int frame = 0; frame < 2; frame++) {
        lens_input in = IN0;
        if (frame == 1)
            in.cursor = (flux_point){20, 20};
        lens_begin(a, &in);
        plain(a, false);
        lens_end(a);
        lens_begin(b, &in);
        skinned_null(b, true);
        lens_end(b);
    }

    CHECK(cmds_equal(find_widget(a, label), find_widget(b, label)));
    lens_destroy(a);
    lens_destroy(b);
}

static void build_button_plain(lens *ui, bool s) {
    (void)s;
    lens_button(ui, &(lens_button_opts){.label = "OK"});
}
static void build_selectable_plain(lens *ui, bool s) {
    (void)s;
    lens_selectable(ui, &(lens_selectable_opts){.label = "Row", .selected = true});
}
static void build_checkbox_plain(lens *ui, bool s) {
    (void)s;
    bool v = true;
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "Check", .value = &v});
}
static void build_slider_plain(lens *ui, bool s) {
    (void)s;
    float v = 25.0f;
    lens_slider(ui, &(lens_slider_opts){.label = "Sli", .value = &v, .min = 0.0f, .max = 100.0f});
}

static void test_null_override_equality(void) {
    check_null_override("OK", build_button_plain, build_button_plain, true, LENS_WIDGET_BUTTON);
    check_null_override("Row", build_selectable_plain, build_selectable_plain, true,
                        LENS_WIDGET_SELECTABLE);
    check_null_override("Check", build_checkbox_plain, build_checkbox_plain, true,
                        LENS_WIDGET_CHECKBOX);
    check_null_override("Sli", build_slider_plain, build_slider_plain, true, LENS_WIDGET_SLIDER);
}

/* ---- custom context skin --------------------------------------------- */

static lens_widget_record g_seen;

static void underline_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_seen = *rec;
    const lens_style_resolved *rs = &rec->style;
    if (rec->state & LENS_STATE_SELECTED)
        lens_skin_rect(ui, node, (flux_rect){0, rec->bounds.h - 2.0f, rec->bounds.w, 2.0f},
                       rs->accent, 0.0f);
    lens_skin_text(ui, node, (flux_rect){rs->padding, 0, 0, -1.0f}, rs->fg, rec->content.label,
                   rs->font_size, 0.0f);
}

static void test_custom_skin_record_and_output(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_get_theme(ui);

    lens_begin(ui, &IN0);
    lens_selectable(ui, &(lens_selectable_opts){.label = "Row", .selected = true});
    lens_end(ui);
    lens_node *def = find_widget(ui, "Row");
    CHECK(def != NULL);
    uint32_t def_cmds = def ? def->cmd_count : 0;

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    lens_set_skin(ui, LENS_WIDGET_SELECTABLE, underline_skin);
    lens_begin(ui, &in);
    lens_selectable(ui, &(lens_selectable_opts){.label = "Row", .selected = true});
    lens_end(ui);
    lens_set_skin(ui, LENS_WIDGET_SELECTABLE, NULL);

    CHECK(g_seen.kind == LENS_WIDGET_SELECTABLE);
    CHECK(g_seen.state & LENS_STATE_SELECTED);
    CHECK(g_seen.state & LENS_STATE_HOVERED);
    CHECK(g_seen.bounds.w > 0.0f && g_seen.bounds.h > 0.0f);
    CHECK(g_seen.last_bounds.w > 0.0f);
    CHECK(g_seen.style.accent == t.color_accent);
    CHECK(g_seen.style_fields == 0);
    CHECK(g_seen.content.label != NULL);
    CHECK(g_seen.hover_t == 1.0f);

    lens_node *n = find_widget(ui, "Row");
    CHECK(n != NULL);
    CHECK(n && n->cmd_count == 2);
    CHECK(def_cmds >= 2);
    if (n && n->cmd_count == 2) {
        CHECK(n->cmds[0].kind == LENS_DRAW_RECT);
        CHECK(n->cmds[0].color == t.color_accent);
        CHECK_NEAR(n->cmds[0].rel.h, 2.0f, 0.0f);
        CHECK(n->cmds[1].kind == LENS_DRAW_TEXT);
    }

    lens_destroy(ui);
}

/* ---- context-wide override ------------------------------------------- */

static bool g_context_skin_ran;

static void flat_button_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_context_skin_ran = true;
    lens_skin_rect(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.bg_pressed, 0.0f);
}

static void hollow_button_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    lens_skin_border(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.fg, 0.0f, 1.0f);
}

static void test_context_skin_override(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    lens_node *n = find_widget(ui, "OK");
    CHECK(n != NULL);
    uint32_t default_cmds = n ? n->cmd_count : 0;
    CHECK(default_cmds >= 2);

    lens_set_skin(ui, LENS_WIDGET_BUTTON, flat_button_skin);
    g_context_skin_ran = false;
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    CHECK(g_context_skin_ran);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_RECT);

    lens_set_skin(ui, LENS_WIDGET_BUTTON, hollow_button_skin);
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_BORDER);

    lens_set_skin(ui, LENS_WIDGET_BUTTON, NULL);
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    CHECK(n && n->cmd_count == default_cmds);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_RECT);

    lens_destroy(ui);
}

/* ---- every kind dispatches and is replaceable ------------------------ */

static lens_widget_record g_probe_seen;
static bool g_probe_ran;

static void probe_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_probe_ran = true;
    g_probe_seen = *rec;
    lens_skin_border(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.accent, 0.0f, 1.0f);
}

typedef void (*probe_build_fn)(lens *ui);
static const char *g_find_name;

static lens_node *find_named(lens *ui) {
    return find_widget(ui, g_find_name);
}
static lens_node *find_sep(lens *ui) {
    return find_widget(ui, "##sep");
}
static lens_node *find_first_child(lens *ui) {
    return lens_node_first_child(lens_root(ui));
}

static void build_label(lens *ui) {
    lens_label(ui, &(lens_label_opts){.text = "Lbl"});
}
static void build_separator(lens *ui) {
    lens_separator(ui, NULL);
}
static void build_icon(lens *ui) {
    lens_icon(ui, &(lens_icon_opts){.id = LENS_ICON_GLOBE, .size = 24.0f});
}
static void build_image(lens *ui) {
    lens_image(ui, &(lens_image_opts){.width = 32.0f, .height = 32.0f});
}
static void build_textedit(lens *ui) {
    static char buf[64] = "abc";
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "Fld"}, .buf = buf, .cap = sizeof buf});
}

static void check_kind(const char *name, lens_widget_kind kind, probe_build_fn build,
                       lens_node *(*find)(lens *ui)) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    lens_node *n = find(ui);
    CHECK(n != NULL);
    uint32_t default_cmds = n ? n->cmd_count : 0;

    lens_set_skin(ui, kind, probe_skin);
    g_probe_ran = false;
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    CHECK(g_probe_ran);
    CHECK(g_probe_seen.kind == kind);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_BORDER);

    lens_set_skin(ui, kind, NULL);
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    CHECK(n && n->cmd_count == default_cmds);
    lens_destroy(ui);
}

static void test_every_kind_dispatches_and_is_replaceable(void) {
    const struct {
        const char *name;
        lens_widget_kind kind;
        probe_build_fn build;
        lens_node *(*find)(lens *ui);
    } cases[] = {
        {"label", LENS_WIDGET_LABEL, build_label, find_named},
        {"separator", LENS_WIDGET_SEPARATOR, build_separator, find_sep},
        {"icon", LENS_WIDGET_ICON, build_icon, find_first_child},
        {"image", LENS_WIDGET_IMAGE, build_image, find_first_child},
        {"textedit", LENS_WIDGET_TEXTEDIT, build_textedit, find_named},
    };
    const char *names[] = {"Lbl", NULL, NULL, NULL, "Fld"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_find_name = names[i];
        check_kind(cases[i].name, cases[i].kind, cases[i].build, cases[i].find);
    }
}

int main(void) {
    test_null_override_equality();
    test_custom_skin_record_and_output();
    test_context_skin_override();
    test_every_kind_dispatches_and_is_replaceable();
    return TEST_REPORT();
}
