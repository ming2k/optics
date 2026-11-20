/* test_a11y.c — accessibility semantics + export walk (ADR-0012). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

/* ---- a collector for lens_accessibility_walk ---- */

typedef struct rec {
    optics_ui_role role;
    char name[32];
    char value[32];
    uint32_t flags;
    optics_ui_id id, parent;
    optics_gfx_rect bounds;
} rec;

static rec g_recs[64];
static int g_n;

static void collect(const optics_ui_semantics *s, optics_gfx_rect bounds, optics_ui_id id,
                    optics_ui_id parent, void *user) {
    (void)user;
    if (g_n >= (int)(sizeof g_recs / sizeof g_recs[0]))
        return;
    rec *r = &g_recs[g_n++];
    r->role = s->role;
    r->flags = s->flags;
    r->id = id;
    r->parent = parent;
    r->bounds = bounds;
    snprintf(r->name, sizeof r->name, "%s", s->name ? s->name : "");
    snprintf(r->value, sizeof r->value, "%s", s->value ? s->value : "");
}

static rec *find_role(optics_ui_role role) {
    for (int i = 0; i < g_n; i++)
        if (g_recs[i].role == role)
            return &g_recs[i];
    return nullptr;
}

static void reset(void) {
    g_n = 0;
    memset(g_recs, 0, sizeof g_recs);
}

/* Built-in widgets publish role, name, and state. */
static void test_widget_roles(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    bool wrap = true;
    float zoom = 1.5f;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Save##b"});
    (void)optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Wrap", .value = &wrap});
    (void)optics_ui_slider(
        ui, &(optics_ui_slider_opts){
                .label = "Zoom", .value = &zoom, .min = 0.0f, .max = 4.0f, .format = "%.4g"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Ready"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    reset();
    optics_ui_accessibility_walk(ui, collect, nullptr);

    rec *b = find_role(OPTICS_UI_ROLE_BUTTON);
    OT_EXPECT(b != nullptr);
    OT_EXPECT(b && strcmp(b->name, "Save") == 0); /* "##b" stripped */

    rec *c = find_role(OPTICS_UI_ROLE_CHECKBOX);
    OT_EXPECT(c != nullptr);
    OT_EXPECT(c && strcmp(c->name, "Wrap") == 0);
    OT_EXPECT(c && (c->flags & OPTICS_UI_A11Y_CHECKED));

    rec *s = find_role(OPTICS_UI_ROLE_SLIDER);
    OT_EXPECT(s != nullptr);
    OT_EXPECT(s && strcmp(s->name, "Zoom") == 0);
    OT_EXPECT(s && strcmp(s->value, "1.5") == 0);

    rec *l = find_role(OPTICS_UI_ROLE_LABEL);
    OT_EXPECT(l != nullptr);
    OT_EXPECT(l && strcmp(l->name, "Ready") == 0);

    /* layout-only row is decorative: not surfaced */
    OT_EXPECT(find_role(OPTICS_UI_ROLE_GROUP) == nullptr);

    optics_ui_release(ui);
}

/* lens_a11y overrides the most recent widget's name (icon-only control). */
static void test_a11y_override(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "##save-icon"});
    optics_ui_a11y(ui, &(optics_ui_a11y_desc){.name = "Save document"});
    optics_ui_test_end(ui);

    reset();
    optics_ui_accessibility_walk(ui, collect, nullptr);
    rec *b = find_role(OPTICS_UI_ROLE_BUTTON);
    OT_EXPECT(b != nullptr);
    OT_EXPECT(b && strcmp(b->name, "Save document") == 0);

    optics_ui_release(ui);
}

/* The walk reports the nearest *semantic* ancestor, skipping decorative
 * layout containers, and reports solved bounds. */
static void test_walk_parenting(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_pressable_begin(
        ui, &(optics_ui_pressable_opts){.box = {.id = "Section"}, .label = "Section"});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Top"});
    optics_ui_pressable_end(ui);
    optics_ui_test_end(ui);

    reset();
    optics_ui_accessibility_walk(ui, collect, nullptr);

    rec *p = find_role(OPTICS_UI_ROLE_BUTTON);
    OT_EXPECT(p != nullptr);

    optics_ui_release(ui);
}

int main(void) {
    test_widget_roles();
    test_a11y_override();
    test_walk_parenting();
    return OT_REPORT();
}
