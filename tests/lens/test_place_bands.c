/* test_place_bands.c — z bands (ADR-0060): closed band set, global
 * emission order (BACKDROP below the base tree, CHROME/POPUP/TOPMOST
 * above, intra-band by registration), EXACT placement as the persistent
 * chrome mode, and BACKDROP's default hit-transparency. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input ZERO_IN = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static inline bool place_begin(lens *ui, const char *id, lens_place_opts opts) {
    opts.box.id = id;
    return lens_place_begin(ui, &opts);
}

static lens_place_opts exact_at(flux_rect rect, lens_band band, flux_color bg) {
    return (lens_place_opts){
        .band = band,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .layout = {.bg = bg},
    };
}

/* ---- EXACT: the persistent-chrome placement mode ------------------- */

/* The body always runs — non-transient nodes have no open state. */
static void test_exact_always_entered(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int body_runs = 0;
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &ZERO_IN);
        if (place_begin(ui, "dock",
                        exact_at((flux_rect){100, 260, 200, 36}, LENS_BAND_CHROME, 0))) {
            body_runs++;
            lens_label(ui, &(lens_label_opts){.text = "tile"});
            lens_place_end(ui);
        }
        lens_end(ui);
    }
    CHECK(body_runs == 2);

    lens_destroy(ui);
}

/* Placement is exactly at the rect's top-left — no below-anchor drop. */
static void test_exact_place_at_rect(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts o = exact_at((flux_rect){40, 50, 120, 24}, LENS_BAND_CHROME, 0);
    o.layout.pad = 4.0f;
    o.layout.min_width = 120.0f;

    /* Frame 1: build so prev_rect is seeded; frame 2: read the settled
     * geometry through the child's prev_rect (one-frame latency). */
    flux_rect label_rect = {0, 0, 0, 0};
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &ZERO_IN);
        if (place_begin(ui, "bar", o)) {
            lens_label(ui, &(lens_label_opts){.text = "x"});
            label_rect = lens_get_response(ui).rect;
            lens_place_end(ui);
        }
        lens_end(ui);
    }

    /* The label sits inside the panel, padded by 4 — so it must be at
     * or below y == 50 (the rect's top). The ANCHORED mode would have
     * dropped it to y >= 74 (50 + 24); EXACT keeps it at 50+pad. */
    CHECK(label_rect.y >= 50.0f);
    CHECK(label_rect.y < 74.0f);
    CHECK(label_rect.x >= 40.0f);

    lens_destroy(ui);
}

/* The supplied rect is a minimum extent, not only a position anchor. This
 * is essential for paint-only nodes with no children (scrims, selection
 * boxes) and must update immediately when callers move/resize the same
 * stable id. */
static void test_rect_extent_tracks_updates(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    lens_id id = lens_current_id(ui, "selection");
    CHECK(place_begin(ui, "selection", exact_at((flux_rect){20, 30, 40, 50}, LENS_BAND_CHROME, 0)));
    lens_place_end(ui);
    lens_end(ui);

    lens_node *node = lens_find(ui, id);
    CHECK(node != NULL);
    flux_rect bounds = lens_node_bounds(node);
    CHECK_NEAR(bounds.x, 20.0f, 0.001f);
    CHECK_NEAR(bounds.y, 30.0f, 0.001f);
    CHECK_NEAR(bounds.w, 40.0f, 0.001f);
    CHECK_NEAR(bounds.h, 50.0f, 0.001f);

    lens_begin(ui, &ZERO_IN);
    CHECK(
        place_begin(ui, "selection", exact_at((flux_rect){80, 60, 120, 90}, LENS_BAND_CHROME, 0)));
    lens_place_end(ui);
    lens_end(ui);

    node = lens_find(ui, id);
    CHECK(node != NULL);
    bounds = lens_node_bounds(node);
    CHECK_NEAR(bounds.x, 80.0f, 0.001f);
    CHECK_NEAR(bounds.y, 60.0f, 0.001f);
    CHECK_NEAR(bounds.w, 120.0f, 0.001f);
    CHECK_NEAR(bounds.h, 90.0f, 0.001f);

    lens_destroy(ui);
}

/* Escape and click-outside must not remove a non-transient node — it has
 * no open state to lose. */
static void test_persistent_not_dismissed(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "wsbar", exact_at((flux_rect){0, 0, 400, 28}, LENS_BAND_CHROME, 0))) {
        lens_label(ui, &(lens_label_opts){.text = "ws1"});
        lens_place_end(ui);
    }
    lens_end(ui);

    lens_input in_esc = ZERO_IN;
    in_esc.key_count = 1;
    in_esc.keys[0] = (lens_key_event){.key = LENS_KEY_ESCAPE, .pressed = true};
    lens_begin(ui, &in_esc);
    int runs_after_esc = 0;
    if (place_begin(ui, "wsbar", exact_at((flux_rect){0, 0, 400, 28}, LENS_BAND_CHROME, 0))) {
        runs_after_esc++;
        lens_label(ui, &(lens_label_opts){.text = "ws1"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(runs_after_esc == 1); /* Escape did not dismiss it */

    lens_input in_click = ZERO_IN;
    in_click.cursor = (flux_point){200, 200}; /* outside the 0..400,0..28 bar */
    in_click.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in_click.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in_click);
    int runs_after_click = 0;
    if (place_begin(ui, "wsbar", exact_at((flux_rect){0, 0, 400, 28}, LENS_BAND_CHROME, 0))) {
        runs_after_click++;
        lens_label(ui, &(lens_label_opts){.text = "ws1"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(runs_after_click == 1); /* click-outside did not dismiss either */

    lens_destroy(ui);
}

/* ---- band-ordered hit-testing -------------------------------------- */

/* A base widget under a CHROME node is occluded: a click on the dock does
 * not fall through to the window behind it. */
static void test_chrome_occludes_base(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts cover = exact_at((flux_rect){0, 0, 200, 200}, LENS_BAND_CHROME, 0xff000000u);
    cover.layout.pad = 8.0f;
    cover.layout.min_width = 200.0f;

    /* Frame 1: base button + chrome node covering it. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "cov", cover)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Top"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);

    /* Frame 2: settled geometry. Press inside the chrome area; the base
     * button must NOT report a click. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 30};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "cov", cover)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Top"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(base_clicked == false);

    lens_destroy(ui);
}

/* Strict band order: a widget inside a CHROME node is occluded by a POPUP
 * node on top of it. */
static void test_popup_occludes_chrome_contents(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts chrome = exact_at((flux_rect){0, 0, 200, 200}, LENS_BAND_CHROME, 0);
    chrome.layout.pad = 8.0f;
    lens_place_opts popup = exact_at((flux_rect){0, 0, 200, 200}, LENS_BAND_POPUP, 0);

    /* Frame 1: settle both nodes. */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "panel", chrome)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "DockBtn"}).clicked;
        lens_place_end(ui);
    }
    if (place_begin(ui, "pop", popup)) {
        lens_label(ui, &(lens_label_opts){.text = "shade"});
        lens_place_end(ui);
    }
    lens_end(ui);

    /* Frame 2: press over the shared area; the chrome button loses. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 30};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool dock_clicked = false;
    if (place_begin(ui, "panel", chrome)) {
        dock_clicked = lens_button(ui, &(lens_button_opts){.label = "DockBtn"}).clicked;
        lens_place_end(ui);
    }
    if (place_begin(ui, "pop", popup)) {
        lens_label(ui, &(lens_label_opts){.text = "shade"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(dock_clicked == false);

    lens_destroy(ui);
}

/* A BACKDROP node renders BELOW the base tree and does not eat hits: a
 * base button under it stays clickable, while a button INSIDE a default
 * (non-interactive) backdrop gets no hits at all. */
static void test_backdrop_hit_transparent(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts bd = exact_at((flux_rect){0, 0, 400, 300}, LENS_BAND_BACKDROP, 0);

    /* Frame 1: settle geometry — base button plus a full-display backdrop
     * carrying its own button. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd", bd)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Ghost"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);

    /* Frame 2/3: press then release over both. The base button (above the
     * backdrop in paint order) still clicks; the backdrop's own button is
     * transparent to hits by default. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd", bd)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Ghost"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    bool ghost_clicked = false;
    if (place_begin(ui, "bd", bd)) {
        ghost_clicked = lens_button(ui, &(lens_button_opts){.label = "Ghost"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(base_clicked == true);   /* BACKDROP does not occlude the base tree */
    CHECK(ghost_clicked == false); /* default BACKDROP swallows no hits */

    lens_destroy(ui);
}

/* The opt-in flag makes a BACKDROP subtree hit-testable. */
static void test_backdrop_interactive_opt_in(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts bd = exact_at((flux_rect){0, 0, 400, 300}, LENS_BAND_BACKDROP, 0);
    bd.interactive = true;

    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", bd)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Deco"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);

    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    if (place_begin(ui, "bd", bd)) {
        (void)lens_button(ui, &(lens_button_opts){.label = "Deco"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked = false;
    if (place_begin(ui, "bd", bd)) {
        clicked = lens_button(ui, &(lens_button_opts){.label = "Deco"}).clicked;
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(clicked == true);

    lens_destroy(ui);
}

/* ---- global emission order, verified with pixels -------------------- */

static uint8_t g_px_channel = 0;

/* Read one channel (0=R, 1=G, 2=B) at (x, y) from the CPU canvas. */
static uint8_t sample(flux_canvas *canvas, uint32_t x, uint32_t y) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = flux_canvas_cpu_pixels(canvas, &w, &h, &stride);
    if (!px || x >= w || y >= h)
        return 0;
    return px[y * stride + x * 4 + g_px_channel];
}

static void render(lens *ui, flux_canvas *canvas) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(canvas, &clear) == FLUX_OK);
    CHECK(lens_render(ui, canvas) == FLUX_OK);
    flux_canvas_cpu_end(canvas);
}

/* Emission order: BACKDROP under the base tree under CHROME under POPUP
 * under TOPMOST; within a band, later registration wins. */
static void test_band_render_order(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 300, 1.0f, &canvas) == FLUX_OK);

    const flux_color RED = 0xFFFF0000u;
    const flux_color GREEN = 0xFF00FF00u;
    const flux_color BLUE = 0xFF0000FFu;
    const flux_color YELLOW = 0xFFFFFF00u;
    const flux_color MAGENTA = 0xFFFF00FFu;
    const flux_rect R = {0, 0, 100, 100};

    g_px_channel = 0; /* red channel */
    /* BACKDROP red + base green row + CHROME blue + POPUP yellow +
     * TOPMOST magenta, all over the same 100x100 spot. The topmost band
     * wins the pixel. */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, LENS_BAND_BACKDROP, RED)))
        lens_place_end(ui);
    lens_size(ui, 100, 100);
    lens_row_begin(ui, &(lens_layout_opts){.bg = GREEN});
    lens_close(ui);
    if (place_begin(ui, "ch", exact_at(R, LENS_BAND_CHROME, BLUE)))
        lens_place_end(ui);
    if (place_begin(ui, "pp", exact_at(R, LENS_BAND_POPUP, YELLOW)))
        lens_place_end(ui);
    if (place_begin(ui, "tm", exact_at(R, LENS_BAND_TOOLTIP, MAGENTA)))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);
    CHECK(sample(canvas, 50, 50) == 255); /* magenta has R=255... */

    /* ...but so do red and yellow; sample the blue channel instead:
     * magenta B=255, red B=0, yellow B=0, green B=0, blue B=255 — blue
     * and magenta tie on B, so use green: magenta G=0, yellow G=255,
     * green G=255, red G=0, blue G=0. Sample G: TOPMOST magenta → 0. */
    g_px_channel = 1;
    CHECK(sample(canvas, 50, 50) == 0); /* magenta: TOPMOST beat POPUP yellow */

    /* Drop TOPMOST: POPUP yellow now wins (G=255). */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, LENS_BAND_BACKDROP, RED)))
        lens_place_end(ui);
    lens_size(ui, 100, 100);
    lens_row_begin(ui, &(lens_layout_opts){.bg = GREEN});
    lens_close(ui);
    if (place_begin(ui, "ch", exact_at(R, LENS_BAND_CHROME, BLUE)))
        lens_place_end(ui);
    if (place_begin(ui, "pp", exact_at(R, LENS_BAND_POPUP, YELLOW)))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);
    CHECK(sample(canvas, 50, 50) == 255); /* yellow G: POPUP beat CHROME+base */

    /* Drop POPUP too: CHROME blue wins (G=0, B=255). */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, LENS_BAND_BACKDROP, RED)))
        lens_place_end(ui);
    lens_size(ui, 100, 100);
    lens_row_begin(ui, &(lens_layout_opts){.bg = GREEN});
    lens_close(ui);
    if (place_begin(ui, "ch", exact_at(R, LENS_BAND_CHROME, BLUE)))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);
    g_px_channel = 2;
    CHECK(sample(canvas, 50, 50) == 255); /* blue B: CHROME beat base+BACKDROP */

    /* Drop CHROME: the base-tree green row beats the BACKDROP. */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, LENS_BAND_BACKDROP, RED)))
        lens_place_end(ui);
    lens_size(ui, 100, 100);
    lens_row_begin(ui, &(lens_layout_opts){.bg = GREEN});
    lens_close(ui);
    lens_end(ui);
    render(ui, canvas);
    g_px_channel = 1;
    CHECK(sample(canvas, 50, 50) == 255); /* base tree over BACKDROP */
    g_px_channel = 0;
    CHECK(sample(canvas, 50, 50) == 0); /* green has no red */

    /* Drop the base row: only the BACKDROP remains. */
    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "bd", exact_at(R, LENS_BAND_BACKDROP, RED)))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);
    CHECK(sample(canvas, 50, 50) == 255); /* BACKDROP still paints, below all */

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

/* Within one band there is no weight: later registration paints (and
 * hit-tests) above earlier. */
static void test_intra_band_registration_order(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 300, 1.0f, &canvas) == FLUX_OK);

    const flux_color CYAN = 0xFF00FFFFu;
    const flux_color YELLOW = 0xFFFFFF00u;
    const flux_rect R = {0, 0, 100, 100};

    lens_begin(ui, &ZERO_IN);
    if (place_begin(ui, "first", exact_at(R, LENS_BAND_POPUP, CYAN)))
        lens_place_end(ui);
    if (place_begin(ui, "second", exact_at(R, LENS_BAND_POPUP, YELLOW)))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);

    g_px_channel = 0;
    CHECK(sample(canvas, 50, 50) == 255); /* both have R... */
    g_px_channel = 2;
    CHECK(sample(canvas, 50, 50) == 0); /* yellow B=0 beat cyan B=255: later wins */

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

/* ADR-0060 reserves LENS_BAND_BASE for flow content: an ABS node asking
 * for BASE is clamped to CHROME by lens_place_begin, so the "what paints
 * above also hit-tests above" invariant can never be violated — it must
 * render above the base tree AND occlude it. */
static void test_base_band_request_clamped_to_chrome(void) {
    lens *ui = NULL;
    flux_canvas *canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(400, 300, 1.0f, &canvas) == FLUX_OK);

    const flux_color BLUE = 0xFF0000FFu;
    lens_place_opts o = exact_at((flux_rect){0, 0, 200, 200}, LENS_BAND_BASE, BLUE);

    /* Frame 1: base button + the BASE-requested node covering it; settle,
     * then verify it paints above the base tree. */
    lens_begin(ui, &ZERO_IN);
    (void)lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        lens_place_end(ui);
    lens_end(ui);
    render(ui, canvas);
    g_px_channel = 2;
    CHECK(sample(canvas, 150, 150) == 255); /* paints above the base tree */

    /* Frames 2-3: press + release over the base button. It must be
     * occluded exactly as if the node were CHROME. */
    lens_input in = ZERO_IN;
    in.cursor = (flux_point){30, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        lens_place_end(ui);
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool base_clicked = lens_button(ui, &(lens_button_opts){.label = "Base"}).clicked;
    if (place_begin(ui, "bd-request", o))
        lens_place_end(ui);
    lens_end(ui);
    CHECK(base_clicked == false); /* and it hit-tests above, too */

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
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
    return TEST_REPORT();
}
