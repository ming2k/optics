/*
 * main.c — Iris application lifecycle, frame orchestration, and entry point for material_gallery.
 */

#include "gallery.h"

static bool on_start(lens *ui, flux_device *device, void *user) {
    gallery_app *app = user;
    gallery_apply_theme(app, ui);
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

    /* Pre-warm material compute pipelines and buffers to eliminate cold-start hitches (ADR-0141) */
    (void)prism_liquid_glass_filter_warmup(app->glass_filter, nullptr);
    (void)prism_frosted_filter_warmup(app->frosted_filter, nullptr);
    (void)prism_acrylic_filter_warmup(app->acrylic_filter, nullptr);
    (void)prism_mica_filter_warmup(app->mica_filter, nullptr);

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

void gallery_emit_output(gallery_app *app, const prism_material_output *out) {
    if (!app || !out || !out->image)
        return;
    if (app->output_count < GALLERY_MAX_OUTPUTS)
        app->outputs[app->output_count++] = *out;
}

void gallery_render_frosted(gallery_app *app, flux_frame *frame, flux_image *blurred,
                            flux_rect bounds, float corner_radius, float saturation) {
    if (!app || !frame || !blurred)
        return;
    lens_material_recipe r = lens_material_recipe_default(LENS_MATERIAL_FROSTED, app->dark_mode);
    prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
    group.shape = (prism_frosted_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    prism_frosted_desc desc = PRISM_FROSTED_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.saturation = saturation;
    desc.tint_strength = r.tint_opacity;

    prism_material_output out = {};
    if (prism_frosted_filter_apply(app->frosted_filter, frame, &desc, &out) == FLUX_OK)
        gallery_emit_output(app, &out);
}

void gallery_render_acrylic(gallery_app *app, flux_frame *frame, flux_image *blurred,
                            flux_rect bounds, float corner_radius, float polarity,
                            float noise_intensity, float border_width) {
    if (!app || !frame || !blurred)
        return;
    lens_material_recipe r = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, app->dark_mode);
    prism_acrylic_group group = PRISM_ACRYLIC_GROUP_INIT;
    group.shape = (prism_acrylic_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    prism_acrylic_desc desc = PRISM_ACRYLIC_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.luminance_plate = polarity;
    desc.tint_strength = r.tint_opacity;
    desc.noise_intensity = noise_intensity;
    desc.border_width = border_width;
    desc.border_alpha = r.border_highlight;

    prism_material_output out = {};
    if (prism_acrylic_filter_apply(app->acrylic_filter, frame, &desc, &out) == FLUX_OK)
        gallery_emit_output(app, &out);
}

void gallery_render_mica(gallery_app *app, flux_frame *frame, flux_image *blurred, flux_rect bounds,
                         float corner_radius, prism_mica_kind kind, float polarity, bool inactive) {
    if (!app || !frame || !blurred)
        return;
    lens_material_recipe r = lens_material_recipe_default(
        kind == PRISM_MICA_ALT ? LENS_MATERIAL_MICA_ALT : LENS_MATERIAL_MICA, app->dark_mode);
    prism_mica_group group = PRISM_MICA_GROUP_INIT;
    group.shape = (prism_mica_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    prism_mica_desc desc = PRISM_MICA_DESC_INIT;
    desc.wallpaper = app->capture;
    desc.blurred_wallpaper = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.kind = kind;
    desc.screen_width = 0.0f;
    desc.screen_height = 0.0f;
    desc.luminosity_plate = polarity;
    desc.tint_color = r.tint & 0x00FFFFFFu;
    desc.fallback_color = r.fallback & 0x00FFFFFFu;
    desc.fallback_weight = inactive ? 1.0f : 0.0f;

    prism_material_output out = {};
    if (prism_mica_filter_apply(app->mica_filter, frame, &desc, &out) == FLUX_OK)
        gallery_emit_output(app, &out);
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
    /* Unconditional on purpose: the engine prunes it when nothing samples it
     * (ADR-0147), so the host does not need a view-dependent guard here. The
     * host still keeps `blurred` as the handle to hand to the material. */
    flux_image *blurred = nullptr;
    {
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = app->capture;
        bd.sigma = 10.0f * scale;
        (void)flux_blur_filter_apply(app->blur_filter, frame, &bd, &blurred);
    }

    /* ===== STEP 3: APPLY MATERIALS ACCORDING TO VIEW MODE ===== */
    app->output_count = 0;

    float header_h = 74.0f * scale;
    gallery_layout layout = {
        .margin_x = 24.0f * scale,
        .header_h = header_h,
        .margin_bottom = 54.0f * scale,
        .avail_w = (float)W - 48.0f * scale,
        .avail_h = (float)H - header_h - 54.0f * scale,
        .polarity = app->dark_mode ? 0.0f : 1.0f,
        .scale = scale,
    };

    switch (app->view) {
    case VIEW_RAW:
        /* Raw backdrop inspection mode: emit zero material layers to isolate background */
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

static void record_image(flux_encoder *enc, flux_image *image, flux_rect dst) {
    flux_geometry geom = flux_geom_rect(dst);
    flux_brush brush = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .tint = flux_color_rgba_premul(255, 255, 255, 255)},
    };
    flux_encoder_draw_geometry(enc, &geom, &brush);
}

static void on_record(flux_encoder *enc, float scale, void *user) {
    (void)scale;
    gallery_app *app = user;

    float W = (float)app->width_px;
    float H = (float)app->height_px;
    if (W <= 0.0f || H <= 0.0f)
        return;

    /* 1. Draw the sharp captured backdrop */
    if (app->capture)
        record_image(enc, app->capture, (flux_rect){0, 0, W, H});

    /* 2. Composite active material layers in declaration order */
    for (uint32_t i = 0; i < app->output_count; ++i) {
        const prism_material_output *out = &app->outputs[i];
        if (out->image)
            record_image(
                enc, out->image,
                (flux_rect){out->origin_x, out->origin_y, (float)out->width, (float)out->height});
    }
}

int main(int argc, char **argv) {
    gallery_app app = {
        .view = VIEW_GLASS,
        .backdrop = BACKDROP_BOUNCING_TV,
        .animating = true,
        .dark_mode = true,
        .inactive_fallback = false,
        .glass_amber = true,
        .frost_vibrancy = true,
        .acrylic_grain = true,
        .mica_alt = false,
        .time = 0.0f,
        .dvd_x = 0.0f,
        .dvd_y = 0.0f,
        .dvd_vx = 0.0f,
        .dvd_vy = 0.0f,
        .dvd_bounce_count = 0,
        .dvd_init = false,
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
        printf(" Hotkeys: [1:Raw 2:Glass 3:Frosted 4:Acrylic 5:Mica], [B] Backdrop, [D] "
               "Dark/Light, [Space] Play/Pause\n");
        printf("=================================================================\n\n");
    }

    return iris_app_run(&(iris_app_opts){
        .title = "Optics Prism Material Gallery (1:Raw 2:Glass 3:Frosted 4:Acrylic 5:Mica)",
        .app_id = "org.optics.showcase.material-gallery",
        .width = 1280,
        .height = 800,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .prepare = on_prepare,
        .record = on_record,
        .user = &app,
    });
}
