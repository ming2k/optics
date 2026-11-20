/* test_place_bands.c — z bands (ADR-0060): closed band set, global
 * emission order (BACKDROP below the base tree, CHROME/POPUP/TOPMOST
 * above, intra-band by registration), EXACT placement as the persistent
 * chrome mode, and BACKDROP's default hit-transparency. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static inline bool place_begin(optics_ui *ui, const char *id, optics_ui_place_opts opts) {
    opts.box.id = id;
    return optics_ui_place_begin(ui, &opts);
}

static optics_ui_place_opts exact_at(optics_gfx_rect rect, optics_ui_band band,
                                     optics_gfx_color bg) {
    return (optics_ui_place_opts){
        .band = band,
        .mode = OPTICS_UI_PLACE_EXACT,
        .rect = rect,
        .layout = {.bg = bg},
    };
}

/* ---- EXACT: the persistent-chrome placement mode ------------------- */

/* The body always runs — non-transient nodes have no open state. */
static void test_exact_always_entered(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    int body_runs = 0;
    for (int frame = 0; frame < 2; frame++) {
        optics_ui_begin(ui, &ZERO_IN);
        if (place_begin(ui, "dock",
                        exact_at((optics_gfx_rect){100, 260, 200, 36}, OPTICS_UI_BAND_CHROME, 0))) {
            body_runs++;
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "tile"});
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
    }
    OT_EXPECT(body_runs == 2);

    optics_ui_release(ui);
}

/* Placement is exactly at the rect's top-left — no below-anchor drop. */
static void test_exact_place_at_rect(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts o = exact_at((optics_gfx_rect){40, 50, 120, 24}, OPTICS_UI_BAND_CHROME, 0);
    o.layout.pad = 4.0f;
    o.layout.box.min_width = 120.0f;

    /* Frame 1: build so prev_rect is seeded; frame 2: read the settled
     * geometry through the child's prev_rect (one-frame latency). */
    optics_gfx_rect label_rect = {0, 0, 0, 0};
    for (int frame = 0; frame < 2; frame++) {
        optics_ui_begin(ui, &ZERO_IN);
        if (place_begin(ui, "bar", o)) {
            optics_ui_label(ui, &(optics_ui_label_opts){.text = "x"});
            label_rect = optics_ui_get_response(ui).rect;
            optics_ui_place_end(ui);
        }
        optics_ui_test_end(ui);
    }

    /* The label sits inside the panel, padded by 4 — so it must be at
     * or below y == 50 (the rect's top). The ANCHORED mode would have
     * dropped it to y >= 74 (50 + 24); EXACT keeps it at 50+pad. */
    OT_EXPECT(label_rect.y >= 50.0f);
    OT_EXPECT(label_rect.y < 74.0f);
    OT_EXPECT(label_rect.x >= 40.0f);

    optics_ui_release(ui);
}

/* The supplied rect is a minimum extent, not only a position anchor. This
 * is essential for paint-only nodes with no children (scrims, selection
 * boxes) and must update immediately when callers move/resize the same
 * stable id. */
static void test_rect_extent_tracks_updates(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    optics_ui_id id = optics_ui_current_id(ui, "selection");
    OT_EXPECT(place_begin(ui, "selection",
                          exact_at((optics_gfx_rect){20, 30, 40, 50}, OPTICS_UI_BAND_CHROME, 0)));
    optics_ui_place_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *node = optics_ui_find(ui, id);
    OT_EXPECT(node != nullptr);
    optics_gfx_rect bounds = optics_ui_node_bounds(node);
    OT_EXPECT_NEAR(bounds.x, 20.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.y, 30.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.w, 40.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.h, 50.0f, 0.001f);

    optics_ui_begin(ui, &ZERO_IN);
    OT_EXPECT(place_begin(ui, "selection",
                          exact_at((optics_gfx_rect){80, 60, 120, 90}, OPTICS_UI_BAND_CHROME, 0)));
    optics_ui_place_end(ui);
    optics_ui_test_end(ui);

    node = optics_ui_find(ui, id);
    OT_EXPECT(node != nullptr);
    bounds = optics_ui_node_bounds(node);
    OT_EXPECT_NEAR(bounds.x, 80.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.y, 60.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.w, 120.0f, 0.001f);
    OT_EXPECT_NEAR(bounds.h, 90.0f, 0.001f);

    optics_ui_release(ui);
}

/* Escape and click-outside must not remove a non-transient node — it has
 * no open state to lose. */
static void test_persistent_not_dismissed(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "wsbar",
                    exact_at((optics_gfx_rect){0, 0, 400, 28}, OPTICS_UI_BAND_CHROME, 0))) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "ws1"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    optics_ui_input in_esc = ZERO_IN;
    in_esc.key_count = 1;
    in_esc.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_ESCAPE, .pressed = true};
    optics_ui_begin(ui, &in_esc);
    int runs_after_esc = 0;
    if (place_begin(ui, "wsbar",
                    exact_at((optics_gfx_rect){0, 0, 400, 28}, OPTICS_UI_BAND_CHROME, 0))) {
        runs_after_esc++;
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "ws1"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(runs_after_esc == 1); /* Escape did not dismiss it */

    optics_ui_input in_click = ZERO_IN;
    in_click.cursor = (optics_gfx_point){200, 200}; /* outside the 0..400,0..28 bar */
    in_click.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in_click.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in_click);
    int runs_after_click = 0;
    if (place_begin(ui, "wsbar",
                    exact_at((optics_gfx_rect){0, 0, 400, 28}, OPTICS_UI_BAND_CHROME, 0))) {
        runs_after_click++;
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "ws1"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(runs_after_click == 1); /* click-outside did not dismiss either */

    optics_ui_release(ui);
}

/* ---- band-ordered hit-testing -------------------------------------- */

/* A base widget under a CHROME node is occluded: a click on the dock does
 * not fall through to the window behind it. */
static void test_chrome_occludes_base(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts cover =
        exact_at((optics_gfx_rect){0, 0, 200, 200}, OPTICS_UI_BAND_CHROME, 0xff000000u);
    cover.layout.pad = 8.0f;
    cover.layout.box.min_width = 200.0f;

    /* Frame 1: base button + chrome node covering it. */
    optics_ui_begin(ui, &ZERO_IN);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "cov", cover)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Top"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    /* Frame 2: settled geometry. Press inside the chrome area; the base
     * button must NOT report a click. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 30};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool base_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "cov", cover)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Top"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(base_clicked == false);

    optics_ui_release(ui);
}

/* Strict band order: a widget inside a CHROME node is occluded by a POPUP
 * node on top of it. */
static void test_popup_occludes_chrome_contents(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts chrome =
        exact_at((optics_gfx_rect){0, 0, 200, 200}, OPTICS_UI_BAND_CHROME, 0);
    chrome.layout.pad = 8.0f;
    optics_ui_place_opts popup =
        exact_at((optics_gfx_rect){0, 0, 200, 200}, OPTICS_UI_BAND_POPUP, 0);

    /* Frame 1: settle both nodes. */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "panel", chrome)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "DockBtn"}).clicked;
        optics_ui_place_end(ui);
    }
    if (place_begin(ui, "pop", popup)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "shade"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    /* Frame 2: press over the shared area; the chrome button loses. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 30};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool dock_clicked = false;
    if (place_begin(ui, "panel", chrome)) {
        dock_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "DockBtn"}).clicked;
        optics_ui_place_end(ui);
    }
    if (place_begin(ui, "pop", popup)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "shade"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(dock_clicked == false);

    optics_ui_release(ui);
}

/* A BACKDROP node renders BELOW the base tree and does not eat hits: a
 * base button under it stays clickable, while a button INSIDE a default
 * (non-interactive) backdrop gets no hits at all. */
static void test_backdrop_hit_transparent(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts bd =
        exact_at((optics_gfx_rect){0, 0, 400, 300}, OPTICS_UI_BAND_BACKDROP, 0);

    /* Frame 1: settle geometry — base button plus a full-display backdrop
     * carrying its own button. */
    optics_ui_begin(ui, &ZERO_IN);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd", bd)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Ghost"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    /* Frame 2/3: press then release over both. The base button (above the
     * backdrop in paint order) still clicks; the backdrop's own button is
     * transparent to hits by default. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd", bd)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Ghost"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool base_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    bool ghost_clicked = false;
    if (place_begin(ui, "bd", bd)) {
        ghost_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Ghost"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(base_clicked == true);   /* BACKDROP does not occlude the base tree */
    OT_EXPECT(ghost_clicked == false); /* default BACKDROP swallows no hits */

    optics_ui_release(ui);
}

/* The opt-in flag makes a BACKDROP subtree hit-testable. */
static void test_backdrop_interactive_opt_in(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_place_opts bd =
        exact_at((optics_gfx_rect){0, 0, 400, 300}, OPTICS_UI_BAND_BACKDROP, 0);
    bd.interactive = true;

    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", bd)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Deco"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    if (place_begin(ui, "bd", bd)) {
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Deco"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);

    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool clicked = false;
    if (place_begin(ui, "bd", bd)) {
        clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Deco"}).clicked;
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(clicked == true);

    optics_ui_release(ui);
}

/* ---- global emission order, verified with pixels -------------------- */

static uint8_t g_px_channel = 0;

/* Read one channel (0=R, 1=G, 2=B) at (x, y) from the CPU canvas. */
static uint8_t sample(optics_gfx_canvas *canvas, uint32_t x, uint32_t y) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_canvas_cpu_pixels(canvas, &w, &h, &stride);
    if (!px || x >= w || y >= h)
        return 0;
    return px[y * stride + x * 4 + g_px_channel];
}

static void render(optics_ui *ui, optics_gfx_canvas *canvas) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
}

/* Emission order: BACKDROP under the base tree under CHROME under POPUP
 * under TOPMOST; within a band, later registration wins. */
static void test_band_render_order(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 300},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_gfx_color RED = 0xFFFF0000u;
    const optics_gfx_color GREEN = 0xFF00FF00u;
    const optics_gfx_color BLUE = 0xFF0000FFu;
    const optics_gfx_color YELLOW = 0xFFFFFF00u;
    const optics_gfx_color MAGENTA = 0xFFFF00FFu;
    const optics_gfx_rect R = {0, 0, 100, 100};

    g_px_channel = 0; /* red channel */
    /* BACKDROP red + base green row + CHROME blue + POPUP yellow +
     * TOPMOST magenta, all over the same 100x100 spot. The topmost band
     * wins the pixel. */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, OPTICS_UI_BAND_BACKDROP, RED)))
        optics_ui_place_end(ui);
    optics_ui_size(ui, 100, 100);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.bg = GREEN});
    optics_ui_close(ui);
    if (place_begin(ui, "ch", exact_at(R, OPTICS_UI_BAND_CHROME, BLUE)))
        optics_ui_place_end(ui);
    if (place_begin(ui, "pp", exact_at(R, OPTICS_UI_BAND_POPUP, YELLOW)))
        optics_ui_place_end(ui);
    if (place_begin(ui, "tm", exact_at(R, OPTICS_UI_BAND_TOOLTIP, MAGENTA)))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* magenta has R=255... */

    /* ...but so do red and yellow; sample the blue channel instead:
     * magenta B=255, red B=0, yellow B=0, green B=0, blue B=255 — blue
     * and magenta tie on B, so use green: magenta G=0, yellow G=255,
     * green G=255, red G=0, blue G=0. Sample G: TOPMOST magenta → 0. */
    g_px_channel = 1;
    OT_EXPECT(sample(canvas, 50, 50) == 0); /* magenta: TOPMOST beat POPUP yellow */

    /* Drop TOPMOST: POPUP yellow now wins (G=255). */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, OPTICS_UI_BAND_BACKDROP, RED)))
        optics_ui_place_end(ui);
    optics_ui_size(ui, 100, 100);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.bg = GREEN});
    optics_ui_close(ui);
    if (place_begin(ui, "ch", exact_at(R, OPTICS_UI_BAND_CHROME, BLUE)))
        optics_ui_place_end(ui);
    if (place_begin(ui, "pp", exact_at(R, OPTICS_UI_BAND_POPUP, YELLOW)))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* yellow G: POPUP beat CHROME+base */

    /* Drop POPUP too: CHROME blue wins (G=0, B=255). */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, OPTICS_UI_BAND_BACKDROP, RED)))
        optics_ui_place_end(ui);
    optics_ui_size(ui, 100, 100);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.bg = GREEN});
    optics_ui_close(ui);
    if (place_begin(ui, "ch", exact_at(R, OPTICS_UI_BAND_CHROME, BLUE)))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    g_px_channel = 2;
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* blue B: CHROME beat base+BACKDROP */

    /* Drop CHROME: the base-tree green row beats the BACKDROP. */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, OPTICS_UI_BAND_BACKDROP, RED)))
        optics_ui_place_end(ui);
    optics_ui_size(ui, 100, 100);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.bg = GREEN});
    optics_ui_close(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    g_px_channel = 1;
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* base tree over BACKDROP */
    g_px_channel = 0;
    OT_EXPECT(sample(canvas, 50, 50) == 0); /* green has no red */

    /* Drop the base row: only the BACKDROP remains. */
    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, OPTICS_UI_BAND_BACKDROP, RED)))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* BACKDROP still paints, below all */

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* Within one band there is no weight: later registration paints (and
 * hit-tests) above earlier. */
static void test_intra_band_registration_order(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 300},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_gfx_color CYAN = 0xFF00FFFFu;
    const optics_gfx_color YELLOW = 0xFFFFFF00u;
    const optics_gfx_rect R = {0, 0, 100, 100};

    optics_ui_begin(ui, &ZERO_IN);
    if (place_begin(ui, "first", exact_at(R, OPTICS_UI_BAND_POPUP, CYAN)))
        optics_ui_place_end(ui);
    if (place_begin(ui, "second", exact_at(R, OPTICS_UI_BAND_POPUP, YELLOW)))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);

    g_px_channel = 0;
    OT_EXPECT(sample(canvas, 50, 50) == 255); /* both have R... */
    g_px_channel = 2;
    OT_EXPECT(sample(canvas, 50, 50) == 0); /* yellow B=0 beat cyan B=255: later wins */

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

/* ADR-0060 reserves LENS_BAND_BASE for flow content: an ABS node asking
 * for BASE is clamped to CHROME by lens_place_begin, so the "what paints
 * above also hit-tests above" invariant can never be violated — it must
 * render above the base tree AND occlude it. */
static void test_base_band_request_clamped_to_chrome(void) {
    optics_ui *ui = nullptr;
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 400,
                                                           .height = 300},
                                 &canvas) == OPTICS_GFX_OK);

    const optics_gfx_color BLUE = 0xFF0000FFu;
    optics_ui_place_opts o = exact_at((optics_gfx_rect){0, 0, 200, 200}, OPTICS_UI_BAND_BASE, BLUE);

    /* Frame 1: base button + the BASE-requested node covering it; settle,
     * then verify it paints above the base tree. */
    optics_ui_begin(ui, &ZERO_IN);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    render(ui, canvas);
    g_px_channel = 2;
    OT_EXPECT(sample(canvas, 150, 150) == 255); /* paints above the base tree */

    /* Frames 2-3: press + release over the base button. It must be
     * occluded exactly as if the node were CHROME. */
    optics_ui_input in = ZERO_IN;
    in.cursor = (optics_gfx_point){30, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool base_clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        optics_ui_place_end(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(base_clicked == false); /* and it hit-tests above, too */

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

int main(void) {
    test_exact_always_entered();
    test_exact_place_at_rect();
    test_rect_extent_tracks_updates();
    test_persistent_not_dismissed();
    test_chrome_occludes_base();
    test_popup_occludes_chrome_contents();
    test_backdrop_hit_transparent();
    test_backdrop_interactive_opt_in();
    test_band_render_order();
    test_intra_band_registration_order();
    test_base_band_request_clamped_to_chrome();
    return OT_REPORT();
}
