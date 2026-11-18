/* test_clip_scope.c — lens_clip_begin/end clips absolutely placed nodes.
 *
 * A placed node deliberately escapes its parent's flow viewport (ADR-0060).
 * A scoped frame clip is a build-time stamp that must follow the node into its
 * band: rendering, the snapshot hit region, and descendants all agree. */

#include "lens_test.h"
#include "optics_test.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {200, 200}, .dt_seconds = 0.016f};

static lens_place_opts placed_at(const char *id, flux_rect rect, lens_band band) {
    lens_place_opts opts = {
        .band = band,
        .mode = LENS_PLACE_EXACT,
        .rect = rect,
        .box.id = id,
    };
    return opts;
}

static bool build_placed_button(lens *ui, const char *id, flux_rect rect, lens_clip_opts *clip) {
    if (clip)
        lens_clip_begin(ui, clip);
    lens_place_open(ui, id);
    lens_place_opts opts = placed_at(id, rect, LENS_BAND_CHROME);
    bool clicked = false;
    if (lens_place_begin(ui, &opts)) {
        clicked = lens_button(ui, &(lens_button_opts){.label = "out"}).clicked;
        lens_place_end(ui);
    }
    if (clip)
        lens_clip_end(ui);
    return clicked;
}

static void test_placed_node_is_clipped_for_hit_testing(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_clip_opts clip = {.rect = {0, 0, 50, 50}};
    flux_rect outside = {100, 100, 60, 24};

    /* Two frames to establish prev_rect. */
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &IN0);
        (void)build_placed_button(ui, "cut", outside, &clip);
        lens_test_end(ui);
    }

    lens_input press = IN0;
    press.cursor = (flux_point){110, 110};
    press.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &press);
    (void)build_placed_button(ui, "cut", outside, &clip);
    lens_test_end(ui);

    lens_input release = IN0;
    release.cursor = (flux_point){110, 110};
    release.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &release);
    bool clicked = build_placed_button(ui, "cut", outside, &clip);
    lens_test_end(ui);
    OT_EXPECT(!clicked);

    /* The same placed button outside a clip is interactive: the test proves
     * the scoped clip, not a placement bug. */
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &IN0);
        (void)build_placed_button(ui, "free", outside, nullptr);
        lens_test_end(ui);
    }
    lens_begin(ui, &press);
    (void)build_placed_button(ui, "free", outside, nullptr);
    lens_test_end(ui);
    lens_begin(ui, &release);
    clicked = build_placed_button(ui, "free", outside, nullptr);
    lens_test_end(ui);
    OT_EXPECT(clicked);

    lens_release(ui);
}

static size_t rendered_ink(lens *ui) {
    flux_canvas *canvas = nullptr;
    OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                     .backend = FLUX_CANVAS_BACKEND_CPU,
                                                     .scale = 1.0f,
                                                     .width = 200,
                                                     .height = 200},
                                 &canvas) == FLUX_OK);
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
              FLUX_OK);
    OT_EXPECT(lens_test_snapshot_render(ui, canvas) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(canvas) == FLUX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
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
    flux_canvas_release(canvas);
    return ink;
}

static void build_placed_label(lens *ui, flux_rect rect, lens_clip_opts *clip) {
    if (clip)
        lens_clip_begin(ui, clip);
    lens_place_open(ui, "label");
    lens_place_opts opts = placed_at("label", rect, LENS_BAND_CHROME);
    if (lens_place_begin(ui, &opts)) {
        lens_label(ui, &(lens_label_opts){.text = "OUTSIDE"});
        lens_place_end(ui);
    }
    if (clip)
        lens_clip_end(ui);
}

static void test_placed_node_is_clipped_for_rendering(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_clip_opts clip = {.rect = {0, 0, 50, 50}};
    flux_rect outside = {100, 100, 90, 30};

    lens_begin(ui, &IN0);
    build_placed_label(ui, outside, &clip);
    lens_test_end(ui);
    OT_EXPECT(rendered_ink(ui) == 0);

    lens_begin(ui, &IN0);
    build_placed_label(ui, outside, nullptr);
    lens_test_end(ui);
    OT_EXPECT(rendered_ink(ui) > 0);

    lens_release(ui);
}

int main(void) {
    test_placed_node_is_clipped_for_hit_testing();
    test_placed_node_is_clipped_for_rendering();
    return OT_REPORT();
}
