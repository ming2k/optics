/*
 * text_hello — shape and draw a UTF-8 run through flux-text on Iris.
 *
 * Demonstrates the ADR-0016 boundary: the host (iris) owns window lifecycle,
 * device, surface, and canvas; flux-text owns shaping (HarfBuzz) and feeds
 * the flux_canvas_draw_glyph_run primitive. BiDi and CJK work because
 * FriBidi + HarfBuzz live in flux-text. Native Wayland cursor and HiDPI
 * scaling work out of the box via Iris.
 */

#include <flux-text/text.h>
#include <flux/canvas.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>
#include <string.h>

typedef struct text_hello_app {
    flux_text *text;
    flux_arena arena;
} text_hello_app;

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    text_hello_app *app = user;

    flux_text_desc tdesc = {.device = device, .scale = 1.0f};
    if (flux_text_create(&tdesc, &app->text) != FLUX_OK) {
        fprintf(stderr, "flux_text_create failed (is fontconfig available?)\n");
        return false;
    }

    if (flux_arena_init(&app->arena, 1u << 20, nullptr) != FLUX_OK) {
        flux_text_release(app->text);
        return false;
    }

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    text_hello_app *app = user;
    if (app->text) {
        flux_text_release(app->text);
        app->text = nullptr;
    }
    flux_arena_deinit(&app->arena);
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

static void on_paint(flux_canvas *canvas, flux_device *device, float scale, void *user) {
    (void)device;
    text_hello_app *app = user;
    if (!app->text)
        return;

    flux_text_set_scale(app->text, scale);

    flux_canvas_save(canvas);
    flux_canvas_scale(canvas, scale, scale);

    const char *s = "hello, flux-text \xe4\xbd\xa0\xe5\xa5\xbd"; /* + "你好" */
    flux_text_style style = {
        .size_px = 40.0f,
        .weight = 0.0f,
        .color = flux_color_rgba(0xe8, 0xe8, 0xe8, 0xff),
        .family = FLUX_TEXT_FAMILY_SANS,
    };
    flux_text_draw(app->text, canvas, &app->arena, 40.0f, 80.0f, s, strlen(s), &style);

    const char *sub = "Native Optics Wayland stack (zero GLFW, HiDPI aware, Esc to quit)";
    flux_text_style sub_style = {
        .size_px = 16.0f,
        .weight = 0.0f,
        .color = flux_color_rgba(0x9a, 0xa0, 0xb4, 0xff),
        .family = FLUX_TEXT_FAMILY_SANS,
    };
    flux_text_draw(app->text, canvas, &app->arena, 40.0f, 130.0f, sub, strlen(sub), &sub_style);

    flux_canvas_restore(canvas);
    flux_arena_reset(&app->arena);
}

int main(void) {
    text_hello_app app = {0};
    printf("flux-text hello — native Iris window (Esc to quit)\n");
    return iris_app_run(&(iris_app_opts){
        .title = "flux text — hello",
        .app_id = "org.optics.flux-text.hello",
        .width = 800,
        .height = 480,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .paint = on_paint,
        .user = &app,
    });
}
