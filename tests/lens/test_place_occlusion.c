/* test_place_occlusion.c — a placed node in a higher band must swallow
 * interactions aimed at widgets beneath it, while its own children stay
 * interactive (ADR-0060: occlusion IS the hit-test order; the eclipse
 * mechanism is deleted). Covers both lensi_interact widgets (buttons) and
 * widgets with their own hit-testing (table rows, scroll wheel routing). */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static lens_place_opts card_opts(flux_rect rect) {
    return (lens_place_opts){
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .layout =
            {
                .pad = 10.0f,
                .gap = 4.0f,
                .bg = 0xFA20202Cu,
                .border = 0x12FFFFFFu,
                .border_width = 1.0f,
                .radius = 12.0f,
                .min_width = 260.0f,
                .cross = LENS_STRETCH,
            },
    };
}

/* Base button at the left; a POPUP card (queue-hover-card style, h=0
 * rect, content-sized) is placed over it. A click landing on the card
 * must never reach the base button; a click on the card's own child
 * button must work. */
static void test_popup_card_occludes_base_widget(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool base_clicked = false;
    bool card_clicked = false;

    for (int frame = 0; frame < 4; ++frame) {
        lens_input in = IN0;
        if (frame == 2)
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        if (frame == 3)
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        if (frame >= 2)
            in.cursor = (flux_point){40.0f, 40.0f};

        lens_begin(ui, &in);

        /* base content: a full-area button the card will cover */
        if (lens_button(ui, "covered playlist row"))
            base_clicked = true;

        /* floating card over the same spot (rect h=0 like wavora's
         * queue hover card) */
        lens_place_begin(ui, "hover-card", card_opts((flux_rect){20.0f, 20.0f, 260.0f, 0.0f}));
        if (lens_button(ui, "card button"))
            card_clicked = true;
        lens_place_end(ui);

        lens_end(ui);
    }

    CHECK(!base_clicked);
    CHECK(card_clicked);
    lens_destroy(ui);
}

static const char *cell_fn(void *user, int row, int col) {
    static char buf[32];
    (void)user;
    snprintf(buf, sizeof buf, "r%dc%d", row, col);
    return buf;
}

/* ---- intra-band occlusion: later registration covers earlier -------- */

static lens_place_opts popup_at(flux_rect rect) {
    return (lens_place_opts){
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .layout = {.pad = 8.0f, .bg = 0xFF202020u, .cross = LENS_STRETCH},
    };
}

/* Two overlapping POPUPs, one band: the later-registered node swallows
 * hover and press over the overlap; the earlier node stays fully
 * interactive where it is NOT covered (ADR-0060: within a band, z is
 * registration order — occlusion is that order, reversed). */
static void test_later_popup_occludes_earlier_same_band(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* "first" covers (0,0)-(200,200); "second" covers (100,0)-(300,200).
     * Buttons stretch to the full popup width, so (150, 20) is inside
     * both buttons' rects while (50, 20) is only inside the first's. */
    const flux_rect R1 = {0, 0, 200, 200};
    const flux_rect R2 = {100, 0, 200, 200};

#define BUILD_TWO()                                                                                \
    do {                                                                                           \
        if (lens_place_begin(ui, "first", popup_at(R1))) {                                         \
            (void)lens_button(ui, "FirstBtn");                                                     \
            first_r = lens_get_response(ui);                                                       \
            lens_place_end(ui);                                                                    \
        }                                                                                          \
        if (lens_place_begin(ui, "second", popup_at(R2))) {                                        \
            (void)lens_button(ui, "SecondBtn");                                                    \
            second_r = lens_get_response(ui);                                                      \
            lens_place_end(ui);                                                                    \
        }                                                                                          \
    } while (0)

    /* Frame 1: settle geometry. */
    lens_response first_r = {0}, second_r = {0};
    lens_begin(ui, &IN0);
    BUILD_TWO();
    lens_end(ui);

    /* Frame 2: hover the overlap — the earlier popup's button must NOT
     * highlight (no double hover, no hover-dwell misfires). */
    lens_input in = IN0;
    in.cursor = (flux_point){150, 20};
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(!first_r.hovered);
    CHECK(second_r.hovered);

    /* Frames 3-4: press + release the overlap. Only the later popup's
     * button may click. */
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(!first_r.clicked);
    CHECK(second_r.clicked);

    /* Frames 5-7: the uncovered part of the earlier popup is unaffected. */
    in = IN0;
    in.cursor = (flux_point){50, 20};
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(first_r.hovered);
    CHECK(!second_r.hovered);
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    BUILD_TWO();
    lens_end(ui);
    CHECK(first_r.clicked);
    CHECK(!second_r.clicked);

#undef BUILD_TWO
    lens_destroy(ui);
}

/* ---- modal dim over an open dropdown (same band, later registration) - */

typedef struct row_probe {
    flux_rect row;
    bool saw;
} row_probe;

static void collect_row_b(const lens_semantics *semantics, flux_rect bounds, lens_id id,
                          lens_id parent, void *user) {
    (void)id;
    (void)parent;
    row_probe *p = user;
    if (semantics->name && strcmp(semantics->name, "B") == 0) {
        p->row = bounds;
        p->saw = true;
    }
}

static void build_dd_modal(lens *ui, int *sel, bool with_modal) {
    const char *items[] = {"A", "B"};
    lens_dropdown(ui, "dd", sel, items, 2);
    if (with_modal) {
        /* Pinned so the dim survives the click frames; the dim is
         * registered after the dropdown's popup in the POPUP band, so it
         * covers it by intra-band registration order. */
        if (lens_modal_begin(ui, "m", (lens_modal_opts){.pinned = true})) {
            lens_label(ui, "body");
            lens_modal_end(ui);
        }
    }
}

static void test_modal_dim_occludes_open_dropdown(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int sel = 0;

    /* f1: idle build. f2/f3: press+release the trigger to open the popup. */
    lens_begin(ui, &IN0);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);

    /* f4: settle the popup and locate row "B". */
    lens_begin(ui, &IN0);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    row_probe p = {0};
    lens_accessibility_walk(ui, collect_row_b, &p);
    CHECK(p.saw && p.row.h > 0.0f);
    flux_point at = {p.row.x + p.row.w * 0.5f, p.row.y + p.row.h * 0.5f};

    /* f5: open the modal over the already-open dropdown. */
    lens_begin(ui, &IN0);
    lens_modal_open(ui, "m");
    build_dd_modal(ui, &sel, true);
    lens_end(ui);

    /* f6/f7: press + release row "B" through the dim. The full-display
     * backdrop (same band, registered later) must swallow the click:
     * the row never fires. */
    in = IN0;
    in.cursor = at;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, true);
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, true);
    lens_end(ui);
    CHECK(sel == 0); /* dim swallowed it */

    /* Control: close the modal, drain one frame for the prev lists, then
     * the same click selects the row. */
    lens_begin(ui, &IN0);
    lens_modal_close(ui, "m");
    build_dd_modal(ui, &sel, true);
    lens_end(ui);
    lens_begin(ui, &IN0);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    in = IN0;
    in.cursor = at;
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_dd_modal(ui, &sel, false);
    lens_end(ui);
    CHECK(sel == 1); /* uncovered: the row fires normally */

    lens_destroy(ui);
}

static const lens_table_column COLS[] = {
    {.title = "Title", .width = 0, .align = LENS_START},
};

static lens_table_result build_table(lens *ui, const lens_input *in, bool with_card) {
    lens_begin(ui, in);
    lens_size(ui, 400, 300);
    lens_table_result r = lens_table(ui, "songs", COLS, 1, 6, cell_fn, NULL,
                                     (lens_table_opts){.row_height = 28, .selectable = true});
    if (with_card) {
        lens_place_begin(ui, "hover-card", card_opts((flux_rect){10.0f, 40.0f, 260.0f, 0.0f}));
        lens_label(ui, "details");
        lens_place_end(ui);
    }
    lens_end(ui);
    return r;
}

/* A selectable table under a POPUP card must not select or scroll
 * through the card (table rows do their own hit-testing). */
static void test_card_occludes_table_rows(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish geometry with the card visible. */
    lens_table_result r = build_table(ui, &IN0, true);
    CHECK(r.selected == -1);

    /* Frame 2: press over a covered row. */
    lens_input in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, true);
    CHECK(r.selected == -1);

    /* Frame 3: release. Still no selection. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, true);
    CHECK(r.selected == -1);

    /* Frame 4: wheel over the covered table must not route to it. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.scroll_y = 3.0f;
    (void)build_table(ui, &in, true);

    /* Drain one card-free frame: a vanished placed node keeps occluding
     * for exactly one frame (same one-frame latency as its appearance —
     * the prev-band lists carry last frame's geometry). */
    (void)build_table(ui, &IN0, false);

    /* Control: without the card, the same press selects the row. */
    in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    r = build_table(ui, &in, false);
    CHECK(r.selected >= 0);

    lens_destroy(ui);
}

/* Wheel scrolling must not reach a scroll area hidden under a card. */
static void test_card_occludes_scroll_wheel(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    float first_row_y = 0.0f;
    for (int frame = 0; frame < 2; ++frame) {
        lens_input in = IN0;
        if (frame == 1) {
            in.cursor = (flux_point){60.0f, 50.0f};
            in.scroll_y = 3.0f;
        }
        lens_begin(ui, &in);
        lens_size(ui, 200.0f, 60.0f);
        lens_scroll_begin(ui, "list");
        for (int i = 0; i < 20; ++i) {
            lens_push_id_int(ui, i);
            lens_label(ui, "row");
            lens_pop_id(ui);
        }
        lens_scroll_end(ui);
        lens_place_begin(ui, "hover-card", card_opts((flux_rect){10.0f, 10.0f, 260.0f, 0.0f}));
        lens_label(ui, "details");
        lens_place_end(ui);
        lens_end(ui);

        lens_node *scroll = lens_node_first_child(lens_root(ui));
        CHECK(scroll != NULL);
        lens_node *first_row = lens_node_first_child(scroll);
        CHECK(first_row != NULL);
        float y = lens_node_bounds(first_row).y;
        if (frame == 0)
            first_row_y = y;
        else
            CHECK(y == first_row_y); /* covered: wheel must not move the list */
    }

    /* Drain one card-free frame before the control case. */
    lens_begin(ui, &IN0);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        lens_push_id_int(ui, i);
        lens_label(ui, "row");
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* Control: without the card, the same wheel delta scrolls the list. */
    lens_input in = IN0;
    in.cursor = (flux_point){60.0f, 50.0f};
    in.scroll_y = -3.0f;
    lens_begin(ui, &in);
    lens_size(ui, 200.0f, 60.0f);
    lens_scroll_begin(ui, "list");
    for (int i = 0; i < 20; ++i) {
        lens_push_id_int(ui, i);
        lens_label(ui, "row");
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
    lens_end(ui);
    lens_node *scroll = lens_node_first_child(lens_root(ui));
    CHECK(scroll != NULL);
    lens_node *first_row = lens_node_first_child(scroll);
    CHECK(first_row != NULL);
    CHECK(lens_node_bounds(first_row).y != first_row_y);

    lens_destroy(ui);
}

int main(void) {
    test_popup_card_occludes_base_widget();
    test_card_occludes_table_rows();
    test_card_occludes_scroll_wheel();
    test_later_popup_occludes_earlier_same_band();
    test_modal_dim_occludes_open_dropdown();
    return TEST_REPORT();
}
