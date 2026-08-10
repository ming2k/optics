/* test_skin.c — the skin layer (ADR-0059; per-call forms retired by
 * ADR-0061, the context is the single override granularity).
 *
 * Covers:
 *   - null-override equality: for every migrated kind, the plain entry
 *     point and an explicit lens_set_skin(kind, lens_default_skin(kind))
 *     emit the identical command stream (the default skin is the pre-skin
 *     emit code verbatim).
 *   - a custom context skin: the record carries correct state/bounds/
 *     style/content, and overriding changes the emitted commands (the
 *     selectable accent rail becomes an underline).
 *   - lens_set_skin context-wide override: applies to the plain entry
 *     points, a later set replaces it, and NULL restores the default.
 *
 * internal.h is included for the retained-node draw-list walk (the node
 * struct is internal; the handles come from the public escape hatch).
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

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

    /* Two frames, the second with a hover, so eased floats are non-trivial. */
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
    lens_button(ui, "OK");
}
static void build_selectable_plain(lens *ui, bool s) {
    (void)s;
    lens_selectable(ui, "Row", true);
}
static void build_selectable_icon_plain(lens *ui, bool s) {
    (void)s;
    lens_selectable_icon(ui, LENS_ICON_GLOBE, "IRow", true);
}
static void build_checkbox_plain(lens *ui, bool s) {
    (void)s;
    bool v = true;
    lens_checkbox(ui, "Check", &v);
}
static void build_switch_plain(lens *ui, bool s) {
    (void)s;
    bool v = true;
    lens_switch(ui, "Sw", &v);
}
static void build_radio_plain(lens *ui, bool s) {
    (void)s;
    int v = 1;
    lens_radio(ui, "Rad", &v, 1);
}
static void build_slider_plain(lens *ui, bool s) {
    (void)s;
    float v = 25.0f;
    lens_slider(ui, "Sli", &v, 0.0f, 100.0f);
}
static void build_slider_v_plain(lens *ui, bool s) {
    (void)s;
    float v = 25.0f;
    lens_slider_vertical(ui, "VSli", &v, 0.0f, 100.0f, 0.0f);
}
static void build_icon_button_plain(lens *ui, bool s) {
    (void)s;
    lens_icon_button_active(ui, LENS_ICON_GLOBE, true);
}
static void build_icon_toggle_plain(lens *ui, bool s) {
    (void)s;
    lens_icon_toggle_button(ui, LENS_ICON_STAR_ROUNDED, LENS_ICON_STAR_ROUNDED_FILLED, 26.0f,
                            true);
}
static void build_icon_badged_plain(lens *ui, bool s) {
    (void)s;
    lens_icon_button_badged(ui, LENS_ICON_REPEAT, "1", 26.0f, true);
}

static void test_null_override_equality(void) {
    /* Every migrated kind: plain form vs an explicit context-wide default
     * skin must emit identical commands. */
    check_null_override("OK", build_button_plain, build_button_plain, true,
                        LENS_WIDGET_BUTTON);
    check_null_override("Row", build_selectable_plain, build_selectable_plain, true,
                        LENS_WIDGET_SELECTABLE);
    check_null_override("IRow", build_selectable_icon_plain, build_selectable_icon_plain,
                        true, LENS_WIDGET_SELECTABLE);
    check_null_override("Check", build_checkbox_plain, build_checkbox_plain, true,
                        LENS_WIDGET_CHECKBOX);
    check_null_override("Sw", build_switch_plain, build_switch_plain, true, LENS_WIDGET_SWITCH);
    check_null_override("Rad", build_radio_plain, build_radio_plain, true, LENS_WIDGET_RADIO);
    check_null_override("Sli", build_slider_plain, build_slider_plain, true, LENS_WIDGET_SLIDER);
    check_null_override("VSli", build_slider_v_plain, build_slider_v_plain, true,
                        LENS_WIDGET_SLIDER);
    check_null_override("##icon124", build_icon_button_plain, build_icon_button_plain, true,
                        LENS_WIDGET_ICON_BUTTON);
    check_null_override("##toggle288:287", build_icon_toggle_plain, build_icon_toggle_plain, true,
                        LENS_WIDGET_ICON_BUTTON);
    check_null_override("##icon201:1", build_icon_badged_plain, build_icon_badged_plain, true,
                        LENS_WIDGET_ICON_BUTTON);
}

/* ---- custom context skin --------------------------------------------- */

static lens_widget_record g_seen; /* record captured by the probe skin */

static void underline_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_seen = *rec;
    /* Replace the stock chrome: a flat accent underline for a selected
     * row, plain text otherwise. No rail, no surface fill. */
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

    /* warm-up frame (default skin), then a hovered frame with the custom
     * per-call skin */
    lens_begin(ui, &IN0);
    lens_selectable(ui, "Row", true);
    lens_end(ui);
    lens_node *def = find_widget(ui, "Row");
    CHECK(def != NULL);
    uint32_t def_cmds = def ? def->cmd_count : 0;

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    lens_set_skin(ui, LENS_WIDGET_SELECTABLE, underline_skin);
    lens_begin(ui, &in);
    lens_selectable(ui, "Row", true);
    lens_end(ui);
    lens_set_skin(ui, LENS_WIDGET_SELECTABLE, NULL);

    /* the record carries kind, state, bounds, style, content */
    CHECK(g_seen.kind == LENS_WIDGET_SELECTABLE);
    CHECK(g_seen.state & LENS_STATE_SELECTED);
    CHECK(g_seen.state & LENS_STATE_HOVERED);
    CHECK(g_seen.bounds.w > 0.0f && g_seen.bounds.h > 0.0f);
    CHECK(g_seen.last_bounds.w > 0.0f); /* warm-up frame arranged it */
    CHECK(g_seen.style.accent == t.color_accent);
    CHECK(g_seen.style_fields == 0); /* NULL instance style */
    CHECK(g_seen.content.label != NULL);
    CHECK(g_seen.hover_t == 1.0f); /* selectable snap-sets hover */

    /* the override changed the emitted commands: underline + text, no
     * surface fill, no rail */
    lens_node *n = find_widget(ui, "Row");
    CHECK(n != NULL);
    CHECK(n && n->cmd_count == 2);
    CHECK(def_cmds >= 2); /* default drew the selected surface + text */
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
    /* one flat rect, nothing else — visibly not the default button */
    lens_skin_rect(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.bg_pressed, 0.0f);
}

static void hollow_button_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    lens_skin_border(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.fg, 0.0f, 1.0f);
}

static void test_context_skin_override(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);
    lens_node *n = find_widget(ui, "OK");
    CHECK(n != NULL);
    uint32_t default_cmds = n ? n->cmd_count : 0;
    CHECK(default_cmds >= 2); /* body + label */

    /* context-wide override applies to the plain entry point */
    lens_set_skin(ui, LENS_WIDGET_BUTTON, flat_button_skin);
    g_context_skin_ran = false;
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);
    CHECK(g_context_skin_ran);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_RECT);

    /* a later set replaces the context skin */
    lens_set_skin(ui, LENS_WIDGET_BUTTON, hollow_button_skin);
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_BORDER);

    /* NULL restores the built-in default */
    lens_set_skin(ui, LENS_WIDGET_BUTTON, NULL);
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);
    CHECK(n && n->cmd_count == default_cmds);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_RECT);
    CHECK(n && n->cmds[1].kind == LENS_DRAW_TEXT);

    lens_destroy(ui);
}

/* ---- every migrated kind dispatches and is replaceable --------------- */
/* The ADR-0059 full sweep: for each kind, a probe skin must (a) receive a
 * record carrying the right kind, and (b) totally replace the emission
 * (the node ends up with exactly the probe's marker command). */

static lens_widget_record g_probe_seen;
static bool g_probe_ran;

static void probe_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_probe_ran = true;
    g_probe_seen = *rec;
    /* one marker border — visibly not any default chrome */
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
    lens_label(ui, "Lbl");
}
static void build_separator(lens *ui) {
    lens_separator(ui);
}
static void build_icon(lens *ui) {
    lens_icon(ui, LENS_ICON_GLOBE, 24.0f);
}
static void build_image(lens *ui) {
    lens_image(ui, NULL, 32.0f, 32.0f);
}
static void build_progress(lens *ui) {
    lens_progress(ui, "load", 0.5f);
}
static void build_textfield(lens *ui) {
    static char buf[64] = "abc";
    lens_textfield(ui, "Fld", buf, sizeof buf);
}
static void build_textarea(lens *ui) {
    static char buf[64] = "l1\nl2";
    lens_textarea(ui, "TxA", buf, sizeof buf, 60.0f);
}
static void build_collapsing(lens *ui) {
    if (lens_collapsing(ui, "Col"))
        lens_close(ui);
}
static void build_tree(lens *ui) {
    if (lens_tree_node(ui, "Tre", false))
        lens_tree_node_end(ui);
}
static const char *probe_cell(void *user, int row, int col) {
    (void)user;
    (void)col;
    static char buf[16];
    snprintf(buf, sizeof buf, "r%d", row);
    return buf;
}
static void build_table(lens *ui) {
    static const lens_table_column cols[1] = {{.title = "T", .width = 0, .align = LENS_START}};
    lens_size(ui, 200, 100);
    lens_table(ui, "Tbl", cols, 1, 4, probe_cell, NULL,
               (lens_table_opts){.row_height = 20, .show_header = true, .selectable = true});
}
static void build_split(lens *ui) {
    if (lens_split_begin(ui, "Spl", LENS_SPLIT_VERTICAL, NULL)) {
        if (lens_split_pane(ui))
            lens_label(ui, "a");
        if (lens_split_pane(ui))
            lens_label(ui, "b");
        lens_split_end(ui);
    }
}
static void build_menu_item(lens *ui) {
    lens_menu_item(ui, "Copy", "Ctrl-C");
}
static void build_dropdown(lens *ui) {
    static const char *items[2] = {"A", "B"};
    int sel = 0;
    lens_dropdown(ui, "Drp", &sel, items, 2);
}
static void build_link(lens *ui) {
    lens_link(ui, "Lnk");
}

static void check_kind(const char *name, lens_widget_kind kind, probe_build_fn build,
                       lens_node *(*find)(lens *ui)) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Two default-skin frames to settle the one-frame-latency state
     * (has_prev, hover eases at the origin cursor); frame 2 is the
     * baseline the restored frame must match. */
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    lens_node *n = find(ui);
    CHECK(n != NULL);
    uint32_t default_cmds = n ? n->cmd_count : 0;

    /* probe frame: the record must carry the right kind and the emission
     * must be replaced by the single marker command */
    lens_set_skin(ui, kind, probe_skin);
    g_probe_ran = false;
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    if (!g_probe_ran || g_probe_seen.kind != kind || !n || n->cmd_count != 1 ||
        n->cmds[0].kind != LENS_DRAW_BORDER)
        fprintf(stderr, "kind probe failed for %s\n", name);
    CHECK(g_probe_ran);
    CHECK(g_probe_seen.kind == kind);
    CHECK(n && n->cmd_count == 1);
    CHECK(n && n->cmds[0].kind == LENS_DRAW_BORDER);

    /* NULL restores the default */
    lens_set_skin(ui, kind, NULL);
    lens_begin(ui, &IN0);
    build(ui);
    lens_end(ui);
    if (n && n->cmd_count != default_cmds)
        fprintf(stderr, "restore mismatch for %s: %u vs %u\n", name, n->cmd_count, default_cmds);
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
        {"progress", LENS_WIDGET_PROGRESS, build_progress, find_named},
        {"textfield", LENS_WIDGET_TEXTFIELD, build_textfield, find_named},
        {"textarea", LENS_WIDGET_TEXTAREA, build_textarea, find_named},
        {"collapsing", LENS_WIDGET_COLLAPSING, build_collapsing, find_named},
        {"tree", LENS_WIDGET_TREE, build_tree, find_named},
        {"table", LENS_WIDGET_TABLE, build_table, find_named},
        {"split", LENS_WIDGET_SPLIT, build_split, find_named},
        {"menu_item", LENS_WIDGET_MENU_ITEM, build_menu_item, find_named},
        {"dropdown", LENS_WIDGET_DROPDOWN, build_dropdown, find_named},
        {"link", LENS_WIDGET_LINK, build_link, find_named},
    };
    const char *names[] = {"Lbl", NULL,  NULL,  NULL,  "load", "Fld", "TxA",
                           "Col", "Tre", "Tbl", "Spl", "Copy", "Drp", "Lnk"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_find_name = names[i];
        check_kind(cases[i].name, cases[i].kind, cases[i].build, cases[i].find);
    }
}

/* Payload spot-checks for the composite/text kinds (correct record data,
 * not just dispatch). */
static void test_composite_records_carry_their_data(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_set_skin(ui, LENS_WIDGET_MENU_ITEM, probe_skin);
    lens_begin(ui, &IN0);
    lens_menu_item(ui, "Copy", "Ctrl-C");
    lens_end(ui);
    CHECK(g_probe_seen.content.label != NULL);
    CHECK(g_probe_seen.content.shortcut != NULL);
    CHECK(!g_probe_seen.content.menu_separator);

    lens_set_skin(ui, LENS_WIDGET_DROPDOWN, probe_skin);
    lens_begin(ui, &IN0);
    {
        static const char *items[2] = {"A", "B"};
        int sel = 1;
        lens_dropdown(ui, "Drp", &sel, items, 2);
    }
    lens_end(ui);
    CHECK(g_probe_seen.content.label != NULL); /* the preview text */
    CHECK(g_probe_seen.content.icon == LENS_ICON_CHEVRON_DOWN); /* closed */
    CHECK(!g_probe_seen.content.popup_open);

    lens_set_skin(ui, LENS_WIDGET_TEXTFIELD, probe_skin);
    lens_begin(ui, &IN0);
    {
        static char buf[64] = "abc";
        lens_textfield(ui, "Fld", buf, sizeof buf);
    }
    lens_end(ui);
    CHECK(g_probe_seen.content.edit_text != NULL);
    CHECK(g_probe_seen.content.sel_rect_count == 0); /* unfocused: no selection */
    CHECK(!g_probe_seen.content.show_caret);

    lens_set_skin(ui, LENS_WIDGET_TABLE, probe_skin);
    lens_begin(ui, &IN0);
    build_table(ui);
    lens_end(ui);
    CHECK(g_probe_seen.content.column_count == 1);
    CHECK(g_probe_seen.content.row_count == 4);
    CHECK(g_probe_seen.content.header_height > 0.0f);

    lens_destroy(ui);
}

int main(void) {
    test_null_override_equality();
    test_custom_skin_record_and_output();
    test_context_skin_override();
    test_every_kind_dispatches_and_is_replaceable();
    test_composite_records_carry_their_data();
    return TEST_REPORT();
}
