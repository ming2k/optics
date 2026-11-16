/*
 * canvas_window — a window with flux content and no UI toolkit.
 *
 * iris depends on flux only: this example links libiris and libflux and
 * nothing else — no lens, no text shaping, no SVG compiler. The window, the
 * event pump and the frame cadence are iris's; the pixels are a recorded
 * flux display list. Press Escape to close.
 */

#include <flux/canvas.h>
#include <iris/iris.h>
#include <math.h>

typedef struct canvas_window_app {
    float time;
    float pointer_x, pointer_y;
} canvas_window_app;

static void fill(flux_encoder *enc, flux_geometry geom, flux_color color) {
    flux_brush brush = flux_brush_solid(color);
    flux_encoder_draw_geometry(enc,
                               &(flux_encoder_draw_geometry_opts){.geom = &geom, .brush = &brush});
}

static void on_record(flux_encoder *enc, float scale, void *user) {
    canvas_window_app *app = user;
    app->time += 1.0f / 60.0f;

    int32_t w = 640, h = 400;
    (void)iris_window_get_geometry(&w, &h);

    flux_encoder_save(enc);
    flux_encoder_scale(enc, &(flux_encoder_scale_opts){.sx = scale, .sy = scale});

    fill(enc, flux_geom_rect((flux_rect){0, 0, (float)w, (float)h}),
         flux_color_rgba_premul(24, 26, 32, 255));

    /* A ring of discs orbiting the centre. */
    const float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    const float orbit = fminf(cx, cy) * 0.6f;
    for (int i = 0; i < 12; i++) {
        const float a = app->time * 0.8f + (float)i * (6.2831853f / 12.0f);
        const uint8_t shade = (uint8_t)(90 + i * 13);
        fill(enc, flux_geom_circle(cx + cosf(a) * orbit, cy + sinf(a) * orbit, 14.0f),
             flux_color_rgba_premul(shade, 150, 255 - shade, 255));
    }

    /* The pointer, straight from the event stream. */
    fill(enc, flux_geom_rrect((flux_rect){app->pointer_x - 18, app->pointer_y - 18, 36, 36}, 8.0f),
         flux_color_rgba_premul(255, 200, 80, 220));

    flux_encoder_restore(enc);
    iris_request_animation_frame();
}

static iris_event_disposition on_event(iris_event *ev, void *user) {
    canvas_window_app *app = user;
    if (ev->type == IRIS_EVENT_POINTER_MOVE) {
        app->pointer_x = ev->pointer.x;
        app->pointer_y = ev->pointer.y;
    } else if (ev->type == IRIS_EVENT_KEY && ev->keyboard.pressed &&
               ev->keyboard.key == IRIS_KEY_ESCAPE) {
        iris_window_close();
    }
    return IRIS_EVENT_PASS;
}

int main(void) {
    canvas_window_app app = {.pointer_x = 320, .pointer_y = 200};
    return iris_app_run(&(iris_app_opts){
        .title = "iris — canvas window",
        .app_id = "org.optics.iris.canvas-window",
        .width = 640,
        .height = 400,
        .on_event = on_event,
        .record = on_record,
        .user = &app,
    });
}
