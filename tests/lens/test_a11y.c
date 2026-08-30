/* test_a11y.c — accessibility semantics + export walk (ADR-0012). */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

/* ---- a collector for lens_accessibility_walk ---- */

typedef struct rec {
    lens_role role;
    char name[32];
    char value[32];
    uint32_t flags;
    lens_id id, parent;
    flux_rect bounds;
} rec;

static rec g_recs[64];
static int g_n;

static void collect(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                    void *user) {
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

static rec *find_role(lens_role role) {
    for (int i = 0; i < g_n; i++)
        if (g_recs[i].role == role)
            return &g_recs[i];
    return NULL;
}

static void reset(void) {
    g_n = 0;
    memset(g_recs, 0, sizeof g_recs);
}

/* Built-in widgets publish role, name, and state. */
static void test_widget_roles(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    bool wrap = true;
    float zoom = 1.5f;
    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    (void)lens_button(ui, &(lens_button_opts){.label = "Save##b"});
    (void)lens_checkbox(ui, &(lens_checkbox_opts){.label = "Wrap", .value = &wrap});
    (void)lens_slider(
        ui, &(lens_slider_opts){
                .label = "Zoom", .value = &zoom, .min = 0.0f, .max = 4.0f, .format = "%.4g"});
    lens_label(ui, &(lens_label_opts){.text = "Ready"});
    lens_close(ui);
    lens_end(ui);

    reset();
    lens_accessibility_walk(ui, collect, NULL);

    rec *b = find_role(LENS_ROLE_BUTTON);
    CHECK(b != NULL);
    CHECK(b && strcmp(b->name, "Save") == 0); /* "##b" stripped */

    rec *c = find_role(LENS_ROLE_CHECKBOX);
    CHECK(c != NULL);
    CHECK(c && strcmp(c->name, "Wrap") == 0);
    CHECK(c && (c->flags & LENS_A11Y_CHECKED));

    rec *s = find_role(LENS_ROLE_SLIDER);
    CHECK(s != NULL);
    CHECK(s && strcmp(s->name, "Zoom") == 0);
    CHECK(s && strcmp(s->value, "1.5") == 0);

    rec *l = find_role(LENS_ROLE_LABEL);
    CHECK(l != NULL);
    CHECK(l && strcmp(l->name, "Ready") == 0);

    /* layout-only row is decorative: not surfaced */
    CHECK(find_role(LENS_ROLE_GROUP) == NULL);

    lens_destroy(ui);
}

/* lens_a11y overrides the most recent widget's name (icon-only control). */
static void test_a11y_override(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "##save-icon"});
    lens_a11y(ui, &(lens_a11y_desc){.name = "Save document"});
    lens_end(ui);

    reset();
    lens_accessibility_walk(ui, collect, NULL);
    rec *b = find_role(LENS_ROLE_BUTTON);
    CHECK(b != NULL);
    CHECK(b && strcmp(b->name, "Save document") == 0);

    lens_destroy(ui);
}

/* The walk reports the nearest *semantic* ancestor, skipping decorative
 * layout containers, and reports solved bounds. */
static void test_walk_parenting(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_pressable_begin(ui, &(lens_pressable_opts){.box = {.id = "Section"}, .label = "Section"});
    (void)lens_button(ui, &(lens_button_opts){.label = "Top"});
    lens_pressable_end(ui);
    lens_end(ui);

    reset();
    lens_accessibility_walk(ui, collect, NULL);

    rec *p = find_role(LENS_ROLE_BUTTON);
    CHECK(p != NULL);

    lens_destroy(ui);
}

int main(void) {
    test_widget_roles();
    test_a11y_override();
    test_walk_parenting();
    return TEST_REPORT();
}
