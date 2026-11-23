/*
 * e2e_public_api — the umbrella header is the only surface an application sees.
 *
 * This test includes <optics/optics.h> and public sub-headers, never a library
 * source path, private internal.h, or a compiled-in translation unit. It proves
 * that the distributed façade links and that the stack's version/lockstep and
 * versioned-first-frame contracts hold in exactly the configuration a consumer
 * installs.
 */

#include "optics_test.h"

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/optics.h>

#include <string.h>

int main(void) {
    static_assert(OPTICS_VERSION_MAJOR == OPTICS_GFX_VERSION_MAJOR,
                  "umbrella and gfx major versions diverge");
    static_assert(OPTICS_VERSION_MINOR == OPTICS_GFX_VERSION_MINOR,
                  "umbrella and gfx minor versions diverge");
    static_assert(OPTICS_VERSION_PATCH == OPTICS_GFX_VERSION_PATCH,
                  "umbrella and gfx patch versions diverge");
    static_assert(OPTICS_VERSION_PATCH == OPTICS_UI_VERSION_PATCH,
                  "umbrella and ui patch versions diverge");
    static_assert(OPTICS_VERSION_PATCH == OPTICS_WINDOW_VERSION_PATCH,
                  "umbrella and window patch versions diverge");

    OT_EXPECT(optics_gfx_version_number() == OPTICS_GFX_VERSION_NUMBER);
    OT_EXPECT(optics_ui_version_number() == OPTICS_UI_VERSION_NUMBER);
    OT_EXPECT(optics_window_version_number() == OPTICS_WINDOW_VERSION_NUMBER);

    int optics_window_major = 0, optics_window_minor = 0, optics_window_patch = 0;
    optics_window_version(&optics_window_major, &optics_window_minor, &optics_window_patch);
    OT_EXPECT(optics_window_major == OPTICS_WINDOW_VERSION_MAJOR);
    OT_EXPECT(optics_window_minor == OPTICS_WINDOW_VERSION_MINOR);
    OT_EXPECT(optics_window_patch == OPTICS_WINDOW_VERSION_PATCH);

    /* A complete frame through the public façade: create, build, publish,
     * activate, submit to the CPU canvas, and read the pixels back. */
    optics_ui *ui = nullptr;
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark()}, &ui) ==
               OPTICS_GFX_OK);
    optics_ui_input input = {.display_size = {160, 80}, .dt_seconds = 1.0f / 60.0f};
    optics_ui_begin(ui, &input);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "optics"});
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_ui_snapshot_activate(ui, snapshot) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc desc = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = 160, .height = 80);
    optics_gfx_canvas *canvas = nullptr;
    OT_REQUIRE(optics_gfx_canvas_create(&desc, &canvas) == OPTICS_GFX_OK);
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(snapshot, canvas) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *pixels = optics_gfx_canvas_read_pixels(canvas, &width, &height, &stride);
    OT_REQUIRE(pixels != nullptr);
    OT_EXPECT(width == 160 && height == 80);
    size_t lit = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4u;
            if (p[0] + p[1] + p[2] > 30)
                lit++;
        }
    }
    OT_EXPECT(lit > 0);

    optics_gfx_canvas_release(canvas);
    optics_ui_snapshot_release(snapshot);
    optics_ui_release(ui);
    return OT_REPORT();
}
