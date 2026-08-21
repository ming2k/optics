/* test_progress.c — progress bar rendering (no interaction). */

#include "test_helpers.h"
#include <lens/lens.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Collected a11y records from the walk (file-scope; C has no closures). */
static const lens_semantics *g_prog[8];
static int g_prog_n;
static void prog_collect(const lens_semantics *s, flux_rect b, lens_id id, lens_id parent,
                         void *user) {
    (void)b;
    (void)id;
    (void)parent;
    (void)user;
    if (g_prog_n < (int)(sizeof g_prog / sizeof g_prog[0]))
        g_prog[g_prog_n++] = s;
}

static void test_progress_renders(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_progress(ui, "Loading", 0.5f);
    lens_end(ui);

    /* The bar is non-interactive, but it must still expose its role and
     * value on the a11y tree — that is its entire contract (ADR-0035). */
    g_prog_n = 0;
    lens_accessibility_walk(ui, prog_collect, NULL);
    CHECK(g_prog_n >= 1);
    CHECK(g_prog[0]->role == LENS_ROLE_PROGRESS);
    CHECK(g_prog[0]->name && strcmp(g_prog[0]->name, "Loading") == 0);
    /* value is a clamped percentage string, e.g. "50%" */
    CHECK(g_prog[0]->value);
    if (g_prog[0]->value)
        CHECK(atof(g_prog[0]->value) == 50.0f);

    lens_destroy(ui);
}

static void test_progress_clamps(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_progress(ui, "Under", -0.5f);
    lens_progress(ui, "Over", 1.5f);
    lens_end(ui);

    /* Out-of-range values must clamp before reaching the a11y value —
     * AT clients read "1.5" as a broken progress bar. */
    g_prog_n = 0;
    lens_accessibility_walk(ui, prog_collect, NULL);
    CHECK(g_prog_n == 2);
    for (int i = 0; i < g_prog_n; i++) {
        float v = g_prog[i]->value ? atof(g_prog[i]->value) : -1.0f;
        CHECK(v >= 0.0f && v <= 100.0f); /* clamped percentage */
    }

    lens_destroy(ui);
}

/* Empty labels used to hash to the raw scope, colliding with the parent
 * container and leaving the container's real children unarranged. The bar
 * itself must get a distinct id and the container must keep its size. */
static void test_progress_empty_label_no_container_collision(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_column_ex(ui, (lens_layout_opts){.pad = 10.0f, .gap = 10.0f, .cross = LENS_STRETCH});
    lens_label(ui, "Before");
    lens_progress(ui, "", 0.5f);
    lens_label(ui, "After");
    lens_close(ui);
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *col = lens_node_first_child(root);
    CHECK(col != NULL);
    flux_rect col_r = lens_node_bounds(col);
    CHECK(col_r.w > 0.0f);
    CHECK(col_r.h > 60.0f); /* two labels + progress + pad + gaps */

    lens_node *before = lens_node_first_child(col);
    lens_node *prog = before ? lens_node_next_sibling(before) : NULL;
    lens_node *after = prog ? lens_node_next_sibling(prog) : NULL;
    CHECK(prog != NULL);
    CHECK(after != NULL);

    flux_rect pr = lens_node_bounds(prog);
    flux_rect ar = lens_node_bounds(after);
    CHECK(pr.w > 0.0f && pr.h > 0.0f);
    CHECK(ar.w > 0.0f && ar.h > 0.0f);
    CHECK(ar.y > pr.y); /* after is laid out below the progress bar */

    lens_destroy(ui);
}

int main(void) {
    test_progress_renders();
    test_progress_clamps();
    test_progress_empty_label_no_container_collision();
    return TEST_REPORT();
}
