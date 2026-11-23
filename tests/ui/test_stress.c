/* test_stress.c — boundary conditions and load: many nodes, deep nesting,
 * arena pressure, store growth. Deterministic boundary tests (no timing,
 * no randomised soak — that shape lives in test_store_spike.c). */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {800, 600}, .dt_seconds = 0.016f};

/* ------------------------------------------------------------------ */
/*  Many nodes force store growth beyond the default 256 slots        */
/* ------------------------------------------------------------------ */
static void test_many_nodes(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: create 500 labels */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 500; i++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item_%d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    OT_EXPECT(root != nullptr);
    /* first child of root is the column; its first child is the first label */
    optics_ui_node *col = optics_ui_node_first_child(root);
    OT_EXPECT(col != nullptr);
    int count = 0;
    for (optics_ui_node *c = optics_ui_node_first_child(col); c; c = optics_ui_node_next_sibling(c))
        count++;
    OT_EXPECT(count == 500);

    /* frame 2: all 500 still present (stable identity) */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 500; i++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item_%d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_overflowed(ui) == false);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Deep nesting: the container stack caps at 64; beyond that, pushes */
/*  are silently ignored. We verify the tree stops growing.           */
/* ------------------------------------------------------------------ */
static void test_deep_nesting(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* 50-deep: should succeed */
    optics_ui_begin(ui, &IN0);
    for (int i = 0; i < 50; i++)
        optics_ui_column_begin(ui, nullptr);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "deep"});
    for (int i = 0; i < 50; i++)
        optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *n = optics_ui_node_first_child(root);
    int depth = 0;
    while (n && optics_ui_node_first_child(n)) {
        depth++;
        n = optics_ui_node_first_child(n);
    }
    OT_EXPECT(depth == 50);

    /* 70-deep: pushes beyond 64 stack entries fail and set overflow. */
    optics_ui_begin(ui, &IN0);
    for (int i = 0; i < 70; i++)
        optics_ui_column_begin(ui, nullptr);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "too deep"});
    for (int i = 0; i < 70; i++)
        optics_ui_close(ui);
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_overflowed(ui) == true);

    root = optics_ui_root(ui);
    n = optics_ui_node_first_child(root);
    depth = 0;
    while (n && optics_ui_node_first_child(n)) {
        depth++;
        n = optics_ui_node_first_child(n);
    }
    /* root (1) + max 63 additional containers = depth 63 */
    OT_EXPECT(depth == 63);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Arena pressure: many draw commands per frame                      */
/* ------------------------------------------------------------------ */
static void test_arena_pressure(void) {
    optics_ui *ui = nullptr;
    /* tiny arena to force pressure */
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){.arena_bytes = 4096}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 200; i++) {
        /* each button pushes multiple draw commands and arena-copies text */
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "X"}).clicked) { /* ignore */
        }
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* With a 4 KiB arena and 200 buttons, overflow is certain: each
     * button arena-copies its label plus at least one draw command, far
     * past 4 KiB. The contract is a REPORTED overflow (never a crash,
     * never silent truncation) — assert the flag, and that a follow-up
     * frame on a fresh arena resets it. */
    OT_EXPECT(optics_ui_overflowed(ui) == true);

    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "one"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_overflowed(ui) == false); /* flag is per frame */

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Rapid node churn: enter / leave / re-enter many ids               */
/* ------------------------------------------------------------------ */
static void test_rapid_churn(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Frame 1: 100 buttons */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 100; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* Frame 2: only even ids remain */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 100; i += 2) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* Frame 3: restore all 100 */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    for (int i = 0; i < 100; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "b%d", i);
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = lbl}).clicked) { /* ignore */
        }
    }
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_overflowed(ui) == false);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Id collision: same label in same scope reuses the same node       */
/* ------------------------------------------------------------------ */
static void test_id_collision_reuses_node(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Dup"});
    optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "Dup"}); /* same id — should resolve to same node */
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *first = optics_ui_node_first_child(root);
    optics_ui_node *second = optics_ui_node_next_sibling(first);
    /* Same label in the same scope derives the same optics_ui_id (ADR-0026),
     * so the second call re-touched the SAME node instead of creating a
     * sibling — the tree holds one child. NOTE: this pins the
     * reconciler's merge behaviour as contract; a caller wanting two
     * buttons must disambiguate the id (optics_ui_push_id). */
    OT_EXPECT(second == nullptr);
    OT_EXPECT(first != nullptr);

    optics_ui_release(ui);
}

int main(void) {
    test_many_nodes();
    test_deep_nesting();
    test_arena_pressure();
    test_rapid_churn();
    test_id_collision_reuses_node();
    return OT_REPORT();
}
