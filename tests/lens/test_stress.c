/* test_stress.c — boundary conditions and load: many nodes, deep nesting,
 * arena pressure, store growth. Deterministic boundary tests (no timing,
 * no randomised soak — that shape lives in test_store_spike.c). */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {800, 600}, .dt_seconds = 0.016f};

/* ------------------------------------------------------------------ */
/*  Many nodes force store growth beyond the default 256 slots        */
/* ------------------------------------------------------------------ */
static void test_many_nodes(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: create 500 labels */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 500; i++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item_%d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_close(ui);
    lens_end(ui);

    lens_node *root = lens_root(ui);
    CHECK(root != NULL);
    /* first child of root is the column; its first child is the first label */
    lens_node *col = lens_node_first_child(root);
    CHECK(col != NULL);
    int count = 0;
    for (lens_node *c = lens_node_first_child(col); c; c = lens_node_next_sibling(c))
        count++;
    CHECK(count == 500);

    /* frame 2: all 500 still present (stable identity) */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 500; i++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item_%d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_close(ui);
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Deep nesting: the container stack caps at 64; beyond that, pushes */
/*  are silently ignored. We verify the tree stops growing.           */
/* ------------------------------------------------------------------ */
static void test_deep_nesting(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* 50-deep: should succeed */
    lens_begin(ui, &IN0);
    for (int i = 0; i < 50; i++)
        lens_column_begin(ui, NULL);
    lens_label(ui, &(lens_label_opts){.text = "deep"});
    for (int i = 0; i < 50; i++)
        lens_close(ui);
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *n = lens_node_first_child(root);
    int depth = 0;
    while (n && lens_node_first_child(n)) {
        depth++;
        n = lens_node_first_child(n);
    }
    CHECK(depth == 50);

    /* 70-deep: pushes beyond 64 stack entries fail and set overflow. */
    lens_begin(ui, &IN0);
    for (int i = 0; i < 70; i++)
        lens_column_begin(ui, NULL);
    lens_label(ui, &(lens_label_opts){.text = "too deep"});
    for (int i = 0; i < 70; i++)
        lens_close(ui);
    lens_end(ui);

    CHECK(lens_overflowed(ui) == true);

    root = lens_root(ui);
    n = lens_node_first_child(root);
    depth = 0;
    while (n && lens_node_first_child(n)) {
        depth++;
        n = lens_node_first_child(n);
    }
    /* root (1) + max 63 additional containers = depth 63 */
    CHECK(depth == 63);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Arena pressure: many draw commands per frame                      */
/* ------------------------------------------------------------------ */
static void test_arena_pressure(void) {
    lens *ui = NULL;
    /* tiny arena to force pressure */
    CHECK(lens_create(&(lens_desc){.arena_bytes = 4096}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 200; i++) {
        /* each button pushes multiple draw commands and arena-copies text */
        if (lens_button(ui, &(lens_button_opts){.label = "X"}).clicked) { /* ignore */
        }
    }
    lens_close(ui);
    lens_end(ui);

    /* With a 4 KiB arena and 200 buttons, overflow is certain: each
     * button arena-copies its label plus at least one draw command, far
     * past 4 KiB. The contract is a REPORTED overflow (never a crash,
     * never silent truncation) — assert the flag, and that a follow-up
     * frame on a fresh arena resets it. */
    CHECK(lens_overflowed(ui) == true);

    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    (void)lens_button(ui, &(lens_button_opts){.label = "one"});
    lens_close(ui);
    lens_end(ui);
    CHECK(lens_overflowed(ui) == false); /* flag is per frame */

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Rapid node churn: enter / leave / re-enter many ids               */
/* ------------------------------------------------------------------ */
static void test_rapid_churn(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: 100 buttons */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 100; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (lens_button(ui, &(lens_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    lens_close(ui);
    lens_end(ui);

    /* Frame 2: only even ids remain */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 100; i += 2) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (lens_button(ui, &(lens_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    lens_close(ui);
    lens_end(ui);

    /* Frame 3: restore all 100 */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, NULL);
    for (int i = 0; i < 100; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (lens_button(ui, &(lens_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    lens_close(ui);
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Id collision: same label in same scope reuses the same node       */
/* ------------------------------------------------------------------ */
static void test_id_collision_reuses_node(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "Dup"});
    lens_button(ui,
                &(lens_button_opts){.label = "Dup"}); /* same id — should resolve to same node */
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *first = lens_node_first_child(root);
    lens_node *second = lens_node_next_sibling(first);
    /* Same label in the same scope derives the same lens_id (ADR-0026),
     * so the second call re-touched the SAME node instead of creating a
     * sibling — the tree holds one child. NOTE: this pins the
     * reconciler's merge behaviour as contract; a caller wanting two
     * buttons must disambiguate the id (lens_push_id). */
    CHECK(second == NULL);
    CHECK(first != NULL);

    lens_destroy(ui);
}

int main(void) {
    test_many_nodes();
    test_deep_nesting();
    test_arena_pressure();
    test_rapid_churn();
    test_id_collision_reuses_node();
    return TEST_REPORT();
}
