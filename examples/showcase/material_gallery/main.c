/*
 * main.c — Iris application lifecycle, frame orchestration, and entry point for material_gallery.
 */

#include "gallery.h"

static bool on_start(lens *ui, flux_device *device, void *user) {
    gallery_app *app = user;
    gallery_apply_theme(ui, app->dark_mode);
    if (flux_arena_init(&app->arena, 1024 * 1024, nullptr) != FLUX_OK)
        return false;

    if (flux_blur_filter_create(device, &app->blur_filter) != FLUX_OK)
        return false;
    if (prism_liquid_glass_filter_create(device, &app->glass_filter) != FLUX_OK)
        return false;
    if (prism_frosted_filter_create(device, &app->frosted_filter) != FLUX_OK)
        return false;
    if (prism_acrylic_filter_create(device, &app->acrylic_filter) != FLUX_OK)
        return false;
    if (prism_mica_filter_create(device, &app->mica_filter) != FLUX_OK)
        return false;

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    gallery_app *app = user;

    if (app->capture) {
        flux_image_release(app->capture);
        app->capture = nullptr;
    }
    prism_mica_filter_release(app->mica_filter);
    prism_acrylic_filter_release(app->acrylic_filter);
    prism_frosted_filter_release(app->frosted_filter);
    prism_liquid_glass_filter_release(app->glass_filter);
    flux_blur_filter_release(app->blur_filter);
    flux_arena_deinit(&app->arena);
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    gallery_app *app = user;
    if (app->smoke_mode) {
        app->smoke_frames++;
        if (app->smoke_frames >= 3) {
            iris_window_close();
            return;
        }
    }
    gallery_ui_build(app, ui, in);
}

static void on_prepare(flux_frame *frame, flux_canvas *canvas, flux_device *device, float scale,
                       void *user) {
    gallery_app *app = user;

    if (app->animating)
        app->time += 0.016f;

    int32_t log_w = 0, log_h = 0;
    if (!iris_window_get_geometry(&log_w, &log_h) || log_w <= 0 || log_h <= 0) {
        log_w = 1280;
        log_h = 800;
    }

    uint32_t W = (uint32_t)lroundf((float)log_w * scale);
    uint32_t H = (uint32_t)lroundf((float)log_h * scale);
    if (W == 0 || H == 0)
        return;

    app->width_px = W;
    app->height_px = H;
    app->scale = scale;

    /* Reallocate capture target on resize or scale change */
    if (!app->capture || flux_image_width(app->capture) != W ||
        flux_image_height(app->capture) != H) {
        if (app->capture)
            flux_image_release(app->capture);
        (void)flux_image_create_render_target(device, W, H, FLUX_FORMAT_RGBA8_UNORM, &app->capture);
    }

    /* ===== STEP 1: CAPTURE backdrop into offscreen render target ===== */
    {
        flux_color clear_bg = flux_color_rgba(15, 12, 24, 255);
        if (app->backdrop == BACKDROP_BLACK) {
            clear_bg = flux_color_rgba(0, 0, 0, 255);
        } else if (app->backdrop == BACKDROP_WHITE) {
            clear_bg = flux_color_rgba(255, 255, 255, 255);
        }

        (void)flux_canvas_begin(
            canvas, &(flux_canvas_pass_desc){
                        .type = FLUX_TYPE_CANVAS_PASS_DESC,
                        .frame = frame,
                        .attachment = {.kind = FLUX_CANVAS_ATTACHMENT_IMAGE, .image = app->capture},
                        .clear_color = &clear_bg});

        gallery_backdrop_render(app, canvas, (float)W, (float)H);
        flux_arena_reset(&app->arena);
        (void)flux_canvas_end(canvas);
    }

    /* ===== STEP 2: BLUR backdrop for optical materials ===== */
    flux_image *blurred = nullptr;
    flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
    bd.input = app->capture;
    bd.sigma = 10.0f * scale;
    (void)flux_blur_filter_apply(app->blur_filter, frame, &bd, &blurred);

    /* ===== STEP 3: APPLY MATERIALS ACCORDING TO VIEW MODE ===== */
    app->glass_out = nullptr;
    app->frost_out = nullptr;
    app->acrylic_out = nullptr;
    app->mica_out = nullptr;

    gallery_layout layout = {
        .margin_x = 24.0f * scale,
        .header_h = 88.0f * scale,
        .margin_bottom = 20.0f * scale,
        .avail_w = (float)W - 48.0f * scale,
        .avail_h = (float)H - 88.0f * scale - 20.0f * scale,
        .polarity = app->dark_mode ? 0.0f : 1.0f,
        .scale = scale,
    };

    switch (app->view) {
    case VIEW_GRID:
        gallery_view_grid_prepare(app, frame, blurred, &layout);
        break;
    case VIEW_GLASS:
        gallery_view_glass_prepare(app, frame, blurred, &layout);
        break;
    case VIEW_FROST:
        gallery_view_frosted_prepare(app, frame, blurred, &layout);
        break;
    case VIEW_ACRYLIC:
        gallery_view_acrylic_prepare(app, frame, blurred, &layout);
        break;
    case VIEW_MICA:
        gallery_view_mica_prepare(app, frame, blurred, &layout);
        break;
    }
}

static void on_paint(flux_canvas *canvas, flux_device *device, float scale, void *user) {
    (void)device;
    (void)scale;
    gallery_app *app = user;

    float W = (float)app->width_px;
    float H = (float)app->height_px;
    if (W <= 0.0f || H <= 0.0f)
        return;

    /* 1. Draw the sharp captured backdrop */
    if (app->capture)
        flux_canvas_draw_image(canvas, app->capture, (flux_rect){0, 0, W, H}, nullptr);

    /* 2. Composite active material layers */
    if (app->glass_out)
        flux_canvas_draw_image(canvas, app->glass_out, (flux_rect){0, 0, W, H}, nullptr);
    if (app->frost_out)
        flux_canvas_draw_image(canvas, app->frost_out, (flux_rect){0, 0, W, H}, nullptr);
    if (app->acrylic_out)
        flux_canvas_draw_image(canvas, app->acrylic_out, (flux_rect){0, 0, W, H}, nullptr);
    if (app->mica_out)
        flux_canvas_draw_image(canvas, app->mica_out, (flux_rect){0, 0, W, H}, nullptr);
}

int main(int argc, char **argv) {
    gallery_app app = {
        .view = VIEW_GRID,
        .backdrop = BACKDROP_DYNAMIC,
        .animating = true,
        .dark_mode = true,
        .inactive_fallback = false,
        .time = 0.0f,
        .smoke_mode = false,
        .smoke_frames = 0,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0)
            app.smoke_mode = true;
    }

    if (!app.smoke_mode) {
        printf("=================================================================\n");
        printf(" Optics Prism Material Gallery (Native Iris Showcase)\n");
        printf(" Native Wayland cursor (wp_cursor_shape_v1) & fractional HiDPI\n");
        printf(
            " Hotkeys: [1..5] Views, [B] Backdrop, [D] Dark/Light, [F] Fallback, [Space] Pause\n");
        printf("=================================================================\n\n");
    }

    return iris_app_run(&(iris_app_opts){
        .title = "Optics Prism Material Gallery (1:Grid 2:Glass 3:Frosted 4:Acrylic 5:Mica)",
        .app_id = "org.optics.showcase.material-gallery",
        .width = 1280,
        .height = 800,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .prepare = on_prepare,
        .paint = on_paint,
        .user = &app,
    });
}
