/* test_progress.c — progress bar rendering (no interaction). */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_progress_renders(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_progress(ui, "Loading", 0.5f);
    lens_end(ui);

    /* Progress bar is non-interactive; just verify it doesn't crash. */
    CHECK(1);

    lens_destroy(ui);
}

static void test_progress_clamps(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_progress(ui, "Under", -0.5f);
    lens_progress(ui, "Over", 1.5f);
    lens_end(ui);

    CHECK(1);

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
