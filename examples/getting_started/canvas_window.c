/*
 * canvas_window.c — native window drawing 2D vectors without optics_ui (ADR-0171).
 *
 * Links only optics_window and optics_gfx (no UI engine, no text shaping, no SVG).
 * Demonstrates recorded display lists, smooth animation pacing, and direct input.
 * Press Esc to close.
 */

#include <optics/gfx/canvas.h>
#include <optics/window.h>

#include <math.h>

typedef struct canvas_window_state {
    float time;
    float pointer_x;
    float pointer_y;
} canvas_window_state;

static void fill_geom(optics_gfx_encoder *enc, optics_gfx_geometry geom, optics_gfx_color color) {
    optics_gfx_brush brush = optics_gfx_brush_solid(color);
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &geom, .brush = &brush});
}

static void on_record(optics_gfx_encoder *enc, float scale, void *user) {
    canvas_window_state *app = user;
    app->time += 1.0f / 60.0f;

    int32_t w = 720, h = 440;
    (void)optics_window_get_geometry(&w, &h);

    optics_gfx_encoder_save(enc);
    optics_gfx_encoder_scale(enc, &(optics_gfx_encoder_scale_opts){.sx = scale, .sy = scale});

    fill_geom(enc, optics_gfx_geom_rect((optics_gfx_rect){0, 0, (float)w, (float)h}),
              optics_gfx_color_rgba_premul(18, 21, 28, 255));

    const float cx = (float)w * 0.5f;
    const float cy = (float)h * 0.5f;
    const float orbit = fminf(cx, cy) * 0.58f;

    for (int i = 0; i < 12; i++) {
        const float angle = app->time * 0.75f + (float)i * (6.2831853f / 12.0f);
        const uint8_t shade = (uint8_t)(85 + i * 14);
        fill_geom(enc,
                  optics_gfx_geom_circle(cx + cosf(angle) * orbit, cy + sinf(angle) * orbit, 13.0f),
                  optics_gfx_color_rgba_premul(shade, 158, 255 - shade, 255));
    }

    fill_geom(enc,
              optics_gfx_geom_rrect(
                  (optics_gfx_rect){app->pointer_x - 18.0f, app->pointer_y - 18.0f, 36.0f, 36.0f},
                  optics_gfx_corner_radii_uniform(10.0f), 0.0f),
              optics_gfx_color_rgba_premul(255, 200, 84, 224));

    optics_gfx_encoder_restore(enc);
    optics_window_request_animation_frame();
}

static optics_window_event_disposition on_event(optics_window_event *ev, void *user) {
    canvas_window_state *app = user;
    if (ev->type == OPTICS_WINDOW_EVENT_POINTER_MOVE) {
        app->pointer_x = ev->pointer.x;
        app->pointer_y = ev->pointer.y;
    } else if (ev->type == OPTICS_WINDOW_EVENT_KEY && ev->keyboard.pressed &&
               ev->keyboard.key == OPTICS_WINDOW_KEY_ESCAPE) {
        optics_window_close();
    }
    return OPTICS_WINDOW_EVENT_PASS;
}

int main(void) {
    canvas_window_state app = {.pointer_x = 360.0f, .pointer_y = 220.0f};
    return optics_window_run(&(optics_window_opts){
        .title = "Optics — Canvas Window",
        .app_id = "org.optics.getting-started.canvas-window",
        .width = 720,
        .height = 440,
        .on_event = on_event,
        .record = on_record,
        .user = &app,
    });
}
