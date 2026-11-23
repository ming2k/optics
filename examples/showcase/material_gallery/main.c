/*
 * main.c — window application lifecycle, frame orchestration, and entry point for material_gallery.
 */

#include "gallery.h"

static bool on_start(optics_ui *ui, optics_gfx_device *device, void *user) {
    gallery_app *app = user;
    gallery_apply_theme(app, ui);
    if (optics_gfx_arena_init(&app->arena, 1024 * 1024, nullptr) != OPTICS_GFX_OK)
        return false;

    if (optics_gfx_blur_filter_create(device, &app->blur_filter) != OPTICS_GFX_OK)
        return false;
    if (optics_material_liquid_glass_filter_create(device, &app->glass_filter) != OPTICS_GFX_OK)
        return false;
    if (optics_material_frosted_filter_create(device, &app->frosted_filter) != OPTICS_GFX_OK)
        return false;
    if (optics_material_acrylic_filter_create(device, &app->acrylic_filter) != OPTICS_GFX_OK)
        return false;
    if (optics_material_mica_filter_create(device, &app->mica_filter) != OPTICS_GFX_OK)
        return false;

    /* Pre-warm material compute pipelines and buffers to eliminate cold-start hitches (ADR-0141) */
    (void)optics_material_liquid_glass_filter_warmup(app->glass_filter, nullptr);
    (void)optics_material_frosted_filter_warmup(app->frosted_filter, nullptr);
    (void)optics_material_acrylic_filter_warmup(app->acrylic_filter, nullptr);
    (void)optics_material_mica_filter_warmup(app->mica_filter, nullptr);

    return true;
}

static void on_stop(optics_ui *ui, optics_gfx_device *device, void *user) {
    (void)ui;
    (void)device;
    gallery_app *app = user;

    if (app->capture) {
        optics_gfx_image_release(app->capture);
        app->capture = nullptr;
    }
    optics_material_mica_filter_release(app->mica_filter);
    optics_material_acrylic_filter_release(app->acrylic_filter);
    optics_material_frosted_filter_release(app->frosted_filter);
    optics_material_liquid_glass_filter_release(app->glass_filter);
    optics_gfx_blur_filter_release(app->blur_filter);
    optics_gfx_arena_deinit(&app->arena);
}

static void on_build(optics_ui *ui, const optics_ui_input *in, void *user) {
    gallery_app *app = user;
    if (app->smoke_mode) {
        app->smoke_frames++;
        if (app->smoke_frames >= 3) {
            optics_window_close();
            return;
        }
    }
    gallery_ui_build(app, ui, in);
}

void gallery_emit_output(gallery_app *app, const optics_material_output *out) {
    if (!app || !out || !out->image)
        return;
    if (app->output_count < GALLERY_MAX_OUTPUTS)
        app->outputs[app->output_count++] = *out;
}

void gallery_render_frosted(gallery_app *app, optics_gfx_frame *frame, optics_gfx_image *blurred,
                            optics_gfx_rect bounds, float corner_radius, float saturation) {
    if (!app || !frame || !blurred)
        return;
    optics_ui_material_recipe r =
        optics_ui_material_recipe_default(OPTICS_UI_MATERIAL_FROSTED, app->dark_mode);
    optics_material_frosted_group group = OPTICS_MATERIAL_FROSTED_GROUP_INIT;
    group.shape = (optics_material_frosted_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    optics_material_frosted_desc desc = OPTICS_MATERIAL_FROSTED_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.saturation = saturation;
    desc.tint_strength = r.tint_opacity;

    optics_material_output out = {};
    if (optics_material_frosted_filter_apply(
            app->frosted_filter, &(optics_material_frosted_filter_apply_opts){
                                     .frame = frame, .desc = &desc, .out = &out}) == OPTICS_GFX_OK)
        gallery_emit_output(app, &out);
}

void gallery_render_acrylic(gallery_app *app, optics_gfx_frame *frame, optics_gfx_image *blurred,
                            optics_gfx_rect bounds, float corner_radius, float polarity,
                            float noise_intensity, float border_width) {
    if (!app || !frame || !blurred)
        return;
    optics_ui_material_recipe r =
        optics_ui_material_recipe_default(OPTICS_UI_MATERIAL_ACRYLIC, app->dark_mode);
    optics_material_acrylic_group group = OPTICS_MATERIAL_ACRYLIC_GROUP_INIT;
    group.shape = (optics_material_acrylic_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.tint_color = r.tint & 0x00FFFFFFu;
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    optics_material_acrylic_desc desc = OPTICS_MATERIAL_ACRYLIC_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = &group;
    desc.group_count = 1;
    desc.luminance_plate = polarity;
    desc.tint_strength = r.tint_opacity;
    desc.noise_intensity = noise_intensity;
    desc.border_width = border_width;
    desc.border_alpha = r.border_highlight;

    optics_material_output out = {};
    if (optics_material_acrylic_filter_apply(
            app->acrylic_filter, &(optics_material_acrylic_filter_apply_opts){
                                     .frame = frame, .desc = &desc, .out = &out}) == OPTICS_GFX_OK)
        gallery_emit_output(app, &out);
}

void gallery_render_mica(gallery_app *app, optics_gfx_frame *frame, optics_gfx_image *blurred,
                         optics_gfx_rect bounds, float corner_radius,
                         optics_material_mica_kind kind, float polarity, bool inactive) {
    if (!app || !frame || !blurred)
        return;
    optics_ui_material_recipe r = optics_ui_material_recipe_default(
        kind == OPTICS_MATERIAL_MICA_ALT ? OPTICS_UI_MATERIAL_MICA_ALT : OPTICS_UI_MATERIAL_MICA,
        app->dark_mode);
    optics_material_mica_group group = OPTICS_MATERIAL_MICA_GROUP_INIT;
    group.shape = (optics_material_mica_shape){.bounds = bounds, .corner_radius = corner_radius};
    group.shadow_alpha = r.shadow_alpha;
    group.shadow_blur = r.shadow_blur * app->scale;
    group.shadow_offset_y = 6.0f * app->scale;

    optics_material_mica_desc desc = OPTICS_MATERIAL_MICA_DESC_INIT;
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

    optics_material_output out = {};
    if (optics_material_mica_filter_apply(
            app->mica_filter, &(optics_material_mica_filter_apply_opts){
                                  .frame = frame, .desc = &desc, .out = &out}) == OPTICS_GFX_OK)
        gallery_emit_output(app, &out);
}

static void on_prepare(optics_gfx_frame *frame, optics_gfx_canvas *canvas,
                       optics_gfx_device *device, float scale, void *user) {
    gallery_app *app = user;

    if (app->animating)
        app->time += 0.016f;

    int32_t log_w = 0, log_h = 0;
    if (!optics_window_get_geometry(&log_w, &log_h) || log_w <= 0 || log_h <= 0) {
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
    if (!app->capture || optics_gfx_image_width(app->capture) != W ||
        optics_gfx_image_height(app->capture) != H) {
        if (app->capture)
            optics_gfx_image_release(app->capture);
        (void)optics_gfx_image_create(
            device,
            &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                     .width = W,
                                     .height = H,
                                     .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                                     .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
            &app->capture);
    }

    /* ===== STEP 1: CAPTURE backdrop into offscreen render target ===== */
    {
        optics_gfx_color clear_bg = optics_gfx_color_rgba(15, 12, 24, 255);
        if (app->backdrop == BACKDROP_BLACK) {
            clear_bg = optics_gfx_color_rgba(0, 0, 0, 255);
        } else if (app->backdrop == BACKDROP_WHITE) {
            clear_bg = optics_gfx_color_rgba(255, 255, 255, 255);
        }

        (void)optics_gfx_canvas_begin(
            canvas,
            &(optics_gfx_canvas_pass_desc){
                .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                .frame = frame,
                .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = app->capture},
                .clear_color = &clear_bg});

        gallery_backdrop_render(app, canvas, (float)W, (float)H);
        optics_gfx_arena_reset(&app->arena);
        (void)optics_gfx_canvas_end(canvas);
    }

    /* ===== STEP 2: BLUR backdrop for optical materials ===== */
    /* Unconditional on purpose: the engine prunes it when nothing samples it
     * (ADR-0147), so the host does not need a view-dependent guard here. The
     * host still keeps `blurred` as the handle to hand to the material. */
    optics_gfx_image *blurred = nullptr;
    {
        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = app->capture;
        bd.sigma = 10.0f * scale;
        (void)optics_gfx_blur_filter_apply(
            app->blur_filter,
            &(optics_gfx_blur_filter_apply_opts){.frame = frame, .desc = &bd, .out = &blurred});
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

static void record_image(optics_gfx_encoder *enc, optics_gfx_image *image, optics_gfx_rect dst) {
    optics_gfx_geometry geom = optics_gfx_geom_rect(dst);
    optics_gfx_brush brush = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image = {.image = image, .tint = optics_gfx_color_rgba_premul(255, 255, 255, 255)},
    };
    optics_gfx_encoder_draw_geometry(
        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &geom, .brush = &brush});
}

static void on_record(optics_gfx_encoder *enc, float scale, void *user) {
    (void)scale;
    gallery_app *app = user;

    float W = (float)app->width_px;
    float H = (float)app->height_px;
    if (W <= 0.0f || H <= 0.0f)
        return;

    /* 1. Draw the sharp captured backdrop */
    if (app->capture)
        record_image(enc, app->capture, (optics_gfx_rect){0, 0, W, H});

    /* 2. Composite active material layers in declaration order */
    for (uint32_t i = 0; i < app->output_count; ++i) {
        const optics_material_output *out = &app->outputs[i];
        if (out->image)
            record_image(enc, out->image,
                         (optics_gfx_rect){out->origin_x, out->origin_y, (float)out->width,
                                           (float)out->height});
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
        printf(" Optics Material Gallery (Native window Showcase)\n");
        printf(" Native Wayland cursor (wp_cursor_shape_v1) & fractional HiDPI\n");
        printf(" Hotkeys: [1:Raw 2:Glass 3:Frosted 4:Acrylic 5:Mica], [B] Backdrop, [D] "
               "Dark/Light, [Space] Play/Pause\n");
        printf("=================================================================\n\n");
    }

    return optics_app_run(&(optics_app_opts){
        .title = "Optics Material Gallery (1:Raw 2:Glass 3:Frosted 4:Acrylic 5:Mica)",
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
