/* test_place.c — transient placed nodes (ADR-0060): open/close persistence,
 * begin gating, anchored placement (with flip), band-order occlusion of base
 * widgets, click-outside and Escape dismissal, and centered placement. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static const optics_ui_place_opts POPUP_AT_ZERO = {
    .band = OPTICS_UI_BAND_POPUP,
    .mode = OPTICS_UI_PLACE_ANCHORED,
    .rect = {0, 0, 0, 0},
    .transient = true,
};

static optics_ui_place_opts anchored(const char *id, optics_gfx_rect anchor, float pad,
                                     float min_width) {
    optics_ui_place_opts o = POPUP_AT_ZERO;
    o.box.id = id;
    o.rect = anchor;
    o.layout.pad = pad;
    o.layout.box.min_width = min_width;
    return o;
}

static void test_open_close_persist(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    OT_EXPECT(optics_ui_place_is_open(ui, "menu") == false);
    optics_ui_place_open(ui, "menu");
    OT_EXPECT(optics_ui_place_is_open(ui, "menu") == true);
    optics_ui_test_end(ui);

    /* persists across frames */
    optics_ui_begin(ui, &ZERO_IN);
    OT_EXPECT(optics_ui_place_is_open(ui, "menu") == true);
    optics_ui_place_close(ui, "menu");
    OT_EXPECT(optics_ui_place_is_open(ui, "menu") == false);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
}

/* The body only enters when the transient node is currently open. */
static void test_begin_gated_by_open_state(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    int body_runs = 0;
    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_opts o1 = anchored("m", (optics_gfx_rect){10, 10, 50, 20}, 4, 80);
    if (optics_ui_place_begin(ui, &o1)) {
        body_runs++;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(body_runs == 0); /* closed → skipped */

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "m");
    optics_ui_place_opts o2 = anchored("m", (optics_gfx_rect){10, 10, 50, 20}, 4, 80);
    if (optics_ui_place_begin(ui, &o2)) {
        body_runs++;
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "item"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(body_runs == 1); /* open → entered */

    optics_ui_release(ui);
}

static void test_open_place_reports_hover_for_its_whole_surface(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "hover-card");
    optics_ui_place_opts o = anchored("hover-card", (optics_gfx_rect){40, 40, 40, 20}, 12, 100);
    if (optics_ui_place_begin(ui, &o)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "value"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    optics_ui_input hover = ZERO_IN;
    hover.cursor = (optics_gfx_point){50, 75};
    optics_ui_begin(ui, &hover);
    OT_EXPECT(optics_ui_place_hovered(ui, "hover-card"));
    if (optics_ui_place_begin(ui, &o)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "value"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    optics_ui_place_close(ui, "hover-card");
    OT_EXPECT(!optics_ui_place_hovered(ui, "hover-card"));
    optics_ui_release(ui);
}

/* Anchored placement: by default, below the anchor; flips above when
 * there's no room. */
static void test_anchor_placement_and_flip(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_gfx_rect bounds_below = {0, 0, 0, 0};
    optics_gfx_rect bounds_flip = {0, 0, 0, 0};

    /* Anchor near the top: the popup drops below it. Two frames so the
     * child's prev_rect settles, then read placement through it. */
    optics_ui_place_opts o_top = anchored("ov", (optics_gfx_rect){50, 20, 100, 24}, 6, 100);
    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "ov");
    if (optics_ui_place_begin(ui, &o_top)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row1"});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row2"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &ZERO_IN);
    if (optics_ui_place_begin(ui, &o_top)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row1"});
        bounds_below = optics_ui_get_response(ui).rect; /* row1's prev_rect */
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row2"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(bounds_below.y >= 44.0f); /* below the anchor (20+24=44) */

    /* Now anchor at the bottom; the popup should flip ABOVE. */
    optics_gfx_rect anchor_bot = {50, 280, 100, 18}; /* y+h = 298 (near 300 bottom) */
    optics_ui_place_opts o_bot = anchored("ov2", anchor_bot, 6, 100);
    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "ov2");
    if (optics_ui_place_begin(ui, &o_bot)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row1-bot"});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row2-bot"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &ZERO_IN);
    if (optics_ui_place_begin(ui, &o_bot)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row1-bot"});
        bounds_flip = optics_ui_get_response(ui).rect;
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "row2-bot"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(bounds_flip.y < anchor_bot.y); /* flipped above */

    optics_ui_release(ui);
}

/* Occlusion IS the hit-test order: a base widget under a POPUP-band node
 * is not hovered/clicked. */
static void test_higher_band_blocks_base(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts cover = anchored("cov", (optics_gfx_rect){0, 0, 200, 200}, 8, 200);
    cover.layout.bg = optics_gfx_color_rgba_premul(0, 0, 0, 255);

    /* Frame 1: build base button + open popup covering it. */
    optics_ui_begin(ui, &ZERO_IN);
    (void)optics_ui_button(ui,
                           &(optics_ui_button_opts){.label = "Base"}); /* hidden under the popup */
    optics_ui_place_open(ui, "cov");
    if (optics_ui_place_begin(ui, &cover)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Top"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    /* Frame 2: layout is settled. Press inside the popup area. The
     * base button must NOT report a click. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 30};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool base_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (optics_ui_place_begin(ui, &cover)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Top"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(base_clicked == false); /* occluded by the POPUP band */

    optics_ui_release(ui);
}

/* Click outside an open transient closes it (with a same-frame grace). */
static void test_click_outside_dismisses(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts o = anchored("m", (optics_gfx_rect){100, 100, 80, 24}, 4, 100);
    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "m");
    if (optics_ui_place_begin(ui, &o)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "a"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_place_is_open(ui, "m") == true);

    /* Frame 2: previous frame's open_frame < current frame, so the grace
     * no longer applies; the outside press closes. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){5, 5};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_place_opts o_zero = anchored("m", (optics_gfx_rect){100, 100, 80, 24}, 4, 0);
    if (optics_ui_place_begin(ui, &o_zero)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "a"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_place_is_open(ui, "m") == false); /* dismissed */

    optics_ui_release(ui);
}

/* Escape closes the top open transient. */
static void test_escape_dismisses_top(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "a");
    optics_ui_place_open(ui, "b");
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_place_is_open(ui, "a"));
    OT_EXPECT(optics_ui_place_is_open(ui, "b"));

    optics_ui_input in = ZERO_IN;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_ESCAPE, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_place_is_open(ui, "b") == false); /* top closed */
    OT_EXPECT(optics_ui_place_is_open(ui, "a") == true);  /* below stays */

    optics_ui_release(ui);
}

/* A CENTERED placed node resolves to the middle of the display (the modal
 * content path). */
static void test_centered_on_display(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "m");
    if (optics_ui_place_begin(ui, &(optics_ui_place_opts){.box = {.id = "m"},
                                                          .band = OPTICS_UI_BAND_MODAL,
                                                          .mode = OPTICS_UI_PLACE_CENTERED,
                                                          .layout = {.box = {.min_width = 200}},
                                                          .transient = true})) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "body"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    optics_ui_node *content = optics_ui_find(ui, optics_ui_current_id(ui, "m"));
    OT_EXPECT(content != nullptr);
    optics_gfx_rect r = optics_ui_node_bounds(content);
    OT_EXPECT_NEAR(r.x + r.w * 0.5f, 200.0f, 0.01f); /* centred in 400 */
    OT_EXPECT_NEAR(r.y + r.h * 0.5f, 150.0f, 0.01f); /* centred in 300 */
    OT_EXPECT_NEAR(r.w, 200.0f, 0.01f);              /* min_width fixed the width */

    optics_ui_release(ui);
}

/* A placed node keeps its parent chain in the one tree: it appears in the
 * a11y walk under its real semantic ancestor, not bolted onto the root. */
static void test_placed_node_parents_into_tree(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_place_open(ui, "pp");
    optics_ui_place_opts o = anchored("pp", (optics_gfx_rect){10, 10, 40, 20}, 4, 60);
    if (optics_ui_place_begin(ui, &o)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "inside"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    /* The placed node is a child of the implicit root container in the
     * tree (its declaration site), and its children hang under it. */
    optics_ui_node *root = optics_ui_root(ui);
    OT_EXPECT(root != nullptr);
    optics_ui_node *popup = optics_ui_find(ui, optics_ui_current_id(ui, "pp"));
    OT_EXPECT(popup != nullptr);
    OT_EXPECT(optics_ui_node_parent(popup) == root);
    optics_ui_node *label = optics_ui_node_first_child(popup);
    OT_EXPECT(label != nullptr);
    OT_EXPECT(optics_ui_node_parent(label) == popup);

    optics_ui_release(ui);
}

int main(void) {
    test_open_close_persist();
    test_begin_gated_by_open_state();
    test_open_place_reports_hover_for_its_whole_surface();
    test_anchor_placement_and_flip();
    test_higher_band_blocks_base();
    test_click_outside_dismisses();
    test_escape_dismisses_top();
    test_centered_on_display();
    test_placed_node_parents_into_tree();
    return OT_REPORT();
}
