/* test_clip_scope.c — lens_clip_begin/end clips absolutely placed nodes.
 *
 * A placed node deliberately escapes its parent's flow viewport (ADR-0060).
 * A scoped frame clip is a build-time stamp that must follow the node into its
 * band: rendering, the snapshot hit region, and descendants all agree. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {200, 200}, .dt_seconds = 0.016f};

static optics_ui_place_opts placed_at(const char *id, optics_gfx_rect rect, optics_ui_band band) {
    optics_ui_place_opts opts = {
        .band = band,
        .mode = OPTICS_UI_PLACE_EXACT,
        .rect = rect,
        .box.id = id,
    };
    return opts;
}

static bool build_placed_button(optics_ui *ui, const char *id, optics_gfx_rect rect,
                                optics_ui_clip_opts *clip) {
    if (clip)
        optics_ui_clip_begin(ui, clip);
    optics_ui_place_open(ui, id);
    optics_ui_place_opts opts = placed_at(id, rect, OPTICS_UI_BAND_CHROME);
    bool clicked = false;
    if (optics_ui_place_begin(ui, &opts)) {
        clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "out"}).clicked;
        optics_ui_place_end(ui);
    }
    if (clip)
        optics_ui_clip_end(ui);
    return clicked;
}

static void test_placed_node_is_clipped_for_hit_testing(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_clip_opts clip = {.rect = {0, 0, 50, 50}};
    optics_gfx_rect outside = {100, 100, 60, 24};

    /* Two frames to establish prev_rect. */
    for (int frame = 0; frame < 2; frame++) {
        optics_ui_begin(ui, &IN0);
        (void)build_placed_button(ui, "cut", outside, &clip);
        optics_ui_test_end(ui);
    }

    optics_ui_input press = IN0;
    press.cursor = (optics_gfx_point){110, 110};
    press.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &press);
    (void)build_placed_button(ui, "cut", outside, &clip);
    optics_ui_test_end(ui);

    optics_ui_input release = IN0;
    release.cursor = (optics_gfx_point){110, 110};
    release.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &release);
    bool clicked = build_placed_button(ui, "cut", outside, &clip);
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* The same placed button outside a clip is interactive: the test proves
     * the scoped clip, not a placement bug. */
    for (int frame = 0; frame < 2; frame++) {
        optics_ui_begin(ui, &IN0);
        (void)build_placed_button(ui, "free", outside, nullptr);
        optics_ui_test_end(ui);
    }
    optics_ui_begin(ui, &press);
    (void)build_placed_button(ui, "free", outside, nullptr);
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &release);
    clicked = build_placed_button(ui, "free", outside, nullptr);
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

static size_t rendered_ink(optics_ui *ui) {
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 200,
                                                           .height = 200},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(canvas, &width, &height, &stride);
    OT_EXPECT(fb != nullptr && width == 200 && height == 200);
    size_t ink = 0;
    if (fb) {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
                if (p[0] || p[1] || p[2])
                    ink++;
            }
        }
    }
    optics_gfx_canvas_release(canvas);
    return ink;
}

static void build_placed_label(optics_ui *ui, optics_gfx_rect rect, optics_ui_clip_opts *clip) {
    if (clip)
        optics_ui_clip_begin(ui, clip);
    optics_ui_place_open(ui, "label");
    optics_ui_place_opts opts = placed_at("label", rect, OPTICS_UI_BAND_CHROME);
    if (optics_ui_place_begin(ui, &opts)) {
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "OUTSIDE"});
        optics_ui_place_end(ui);
    }
    if (clip)
        optics_ui_clip_end(ui);
}

static void test_placed_node_is_clipped_for_rendering(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_clip_opts clip = {.rect = {0, 0, 50, 50}};
    optics_gfx_rect outside = {100, 100, 90, 30};

    optics_ui_begin(ui, &IN0);
    build_placed_label(ui, outside, &clip);
    optics_ui_test_end(ui);
    OT_EXPECT(rendered_ink(ui) == 0);

    optics_ui_begin(ui, &IN0);
    build_placed_label(ui, outside, nullptr);
    optics_ui_test_end(ui);
    OT_EXPECT(rendered_ink(ui) > 0);

    optics_ui_release(ui);
}

int main(void) {
    test_placed_node_is_clipped_for_hit_testing();
    test_placed_node_is_clipped_for_rendering();
    return OT_REPORT();
}
