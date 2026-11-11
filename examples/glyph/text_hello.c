/*
 * text_hello — shape and draw a UTF-8 run through glyph on Iris.
 *
 * Demonstrates the ADR-0016 / ADR-0106 boundary: the host (iris) owns window lifecycle,
 * device, surface, and canvas; glyph owns shaping (HarfBuzz) and feeds
 * the flux_canvas_draw_glyph_run primitive. BiDi and CJK work because
 * FriBidi + HarfBuzz live in glyph. Native Wayland cursor and HiDPI
 * scaling work out of the box via Iris.
 */

#include <flux/canvas.h>
#include <flux/flux.h>
#include <glyph/glyph.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>
#include <string.h>

typedef struct text_hello_app {
    glyph_ctx *text;
} text_hello_app;

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    text_hello_app *app = user;

    glyph_desc tdesc = {.device = device, .scale = 1.0f};
    if (glyph_create(&tdesc, &app->text) != FLUX_OK) {
        fprintf(stderr, "glyph_create failed (is fontconfig available?)\n");
        return false;
    }

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    text_hello_app *app = user;
    if (app->text) {
        glyph_release(app->text);
        app->text = nullptr;
    }
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    (void)ui;
    (void)user;
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && (in->keys[k].key == LENS_KEY_ESCAPE || in->keys[k].key == 'q' ||
                                    in->keys[k].key == 'Q')) {
            iris_window_close();
            return;
        }
    }
}

static void on_record(flux_encoder *enc, float scale, void *user) {
    text_hello_app *app = user;
    if (!app->text)
        return;

    glyph_set_scale(app->text, scale);

    flux_encoder_save(enc);
    flux_encoder_scale(enc, scale, scale);

    const char *s = "hello, glyph \xe4\xbd\xa0\xe5\xa5\xbd"; /* + "你好" */
    glyph_style style = {
        .size_px = 40.0f,
        .weight = 0.0f,
        .color = flux_color_rgba(0xe8, 0xe8, 0xe8, 0xff),
        .family = GLYPH_FAMILY_SANS,
    };
    (void)glyph_record(
        app->text, enc,
        &(glyph_record_desc){.x = 40.0f, .y = 80.0f, .utf8 = s, .len = strlen(s), .style = style});

    const char *sub = "Native Optics Wayland stack (zero GLFW, HiDPI aware, Esc to quit)";
    glyph_style sub_style = {
        .size_px = 16.0f,
        .weight = 0.0f,
        .color = flux_color_rgba(0x9a, 0xa0, 0xb4, 0xff),
        .family = GLYPH_FAMILY_SANS,
    };
    (void)glyph_record(
        app->text, enc,
        &(glyph_record_desc){
            .x = 40.0f, .y = 130.0f, .utf8 = sub, .len = strlen(sub), .style = sub_style});

    flux_encoder_restore(enc);
}

int main(void) {
    text_hello_app app = {0};
    printf("glyph hello — native Iris window (Esc to quit)\n");
    return iris_app_run(&(iris_app_opts){
        .title = "glyph — hello",
        .app_id = "org.optics.glyph.hello",
        .width = 800,
        .height = 480,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .record = on_record,
        .user = &app,
    });
}
