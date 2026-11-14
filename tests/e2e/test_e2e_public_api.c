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

#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <optics/optics.h>

#include <string.h>

int main(void) {
    static_assert(OPTICS_VERSION_MAJOR == FLUX_VERSION_MAJOR,
                  "umbrella and flux major versions diverge");
    static_assert(OPTICS_VERSION_MINOR == FLUX_VERSION_MINOR,
                  "umbrella and flux minor versions diverge");
    static_assert(OPTICS_VERSION_PATCH == FLUX_VERSION_PATCH,
                  "umbrella and flux patch versions diverge");
    static_assert(OPTICS_VERSION_PATCH == LENS_VERSION_PATCH,
                  "umbrella and lens patch versions diverge");
    static_assert(OPTICS_VERSION_PATCH == IRIS_VERSION_PATCH,
                  "umbrella and iris patch versions diverge");

    OT_EXPECT(flux_version_number() == FLUX_VERSION_NUMBER);
    OT_EXPECT(lens_version_number() == LENS_VERSION_NUMBER);
    OT_EXPECT(iris_version_number() == IRIS_VERSION_NUMBER);

    int iris_major = 0, iris_minor = 0, iris_patch = 0;
    iris_version(&iris_major, &iris_minor, &iris_patch);
    OT_EXPECT(iris_major == IRIS_VERSION_MAJOR);
    OT_EXPECT(iris_minor == IRIS_VERSION_MINOR);
    OT_EXPECT(iris_patch == IRIS_VERSION_PATCH);

    /* A complete frame through the public façade: create, build, publish,
     * activate, submit to the CPU canvas, and read the pixels back. */
    lens *ui = nullptr;
    OT_REQUIRE(lens_create(&(lens_desc){.theme = lens_theme_dark()}, &ui) == FLUX_OK);
    lens_input input = {.display_size = {160, 80}, .dt_seconds = 1.0f / 60.0f};
    lens_begin(ui, &input);
    lens_label(ui, &(lens_label_opts){.text = "optics"});
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    OT_EXPECT(!lens_overflowed(ui));

    lens_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    OT_REQUIRE(lens_snapshot_activate(ui, snapshot) == FLUX_OK);

    flux_canvas_desc desc =
        FLUX_INIT(CANVAS_DESC, .backend = FLUX_CANVAS_BACKEND_CPU, .width = 160, .height = 80);
    flux_canvas *canvas = nullptr;
    OT_REQUIRE(flux_canvas_create(&desc, &canvas) == FLUX_OK);
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(
        flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                           .clear_color = &clear}) == FLUX_OK);
    OT_EXPECT(lens_snapshot_submit(snapshot, canvas) == FLUX_OK);
    OT_REQUIRE(flux_canvas_end(canvas) == FLUX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *pixels = flux_canvas_read_pixels(canvas, &width, &height, &stride);
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

    flux_canvas_release(canvas);
    lens_snapshot_release(snapshot);
    lens_release(ui);
    return OT_REPORT();
}
