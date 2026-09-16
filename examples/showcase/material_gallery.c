/*
 * material_gallery — interactive comparative showcase for all Prism materials.
 *
 * Demonstrates the optical, algorithmic, and visual differences between
 * modern UI materials under a unified dynamic backdrop, built completely
 * on Optics's native Iris + Lens + Prism + Flux stack (zero GLFW, native Wayland
 * wp_cursor_shape_v1 cursor theming, and full fractional HiDPI scaling).
 *
 * Materials:
 *   1. Liquid Glass (visionOS / Convex Optics): physical IOR refraction,
 *      chromatic dispersion, specular rim lighting, and focus field.
 *   2. Frosted Glass (macOS / Vibrancy): high-purity dual-Kawase blur and
 *      color saturation boost (vibrancy).
 *   3. Acrylic (Windows Fluent): multi-layer composite with dual-Kawase blur,
 *      luminance plate balancing, 1px SDF border highlight, and procedural grain.
 *   4. Mica & Mica Alt (Windows 11): screen-anchored foundation material with
 *      soft blur, theme tinting, mineral dither, and inactive fallback state.
 *
 * Controls:
 *   [1] 4-Up Grid comparison view (all 4 materials side-by-side)
 *   [2] Liquid Glass fullscreen focus
 *   [3] Frosted Glass fullscreen focus
 *   [4] Acrylic fullscreen focus
 *   [5] Mica & Mica Alt fullscreen focus
 *   [D] Toggle Dark (Smoke) / Light (Pearl) theme plate polarity
 *   [F] Toggle Inactive state fallback on Mica
 *   [Space] Pause / Resume backdrop animation
 *   [Esc] or [Q] Exit
 */

#include <flux/effect.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <iris/cursor.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>
#include <prism/prism.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum view_mode {
    VIEW_GRID = 1,
    VIEW_GLASS = 2,
    VIEW_FROST = 3,
    VIEW_ACRYLIC = 4,
    VIEW_MICA = 5,
} view_mode;

typedef struct gallery_app {
    view_mode view;
    bool animating;
    bool dark_mode;
    bool inactive_fallback;
    float time;

    /* GPU resources */
    flux_arena arena;
    flux_image *capture;
    flux_blur_filter *blur_filter;
    prism_liquid_glass_filter *glass_filter;
    prism_frosted_filter *frosted_filter;
    prism_acrylic_filter *acrylic_filter;
    prism_mica_filter *mica_filter;

    /* Frame outputs */
    flux_image *glass_out;
    flux_image *frost_out;
    flux_image *acrylic_out;
    flux_image *mica_out;

    uint32_t width_px;
    uint32_t height_px;
    float scale;
} gallery_app;

/* Draw a rich, animated backdrop: flowing radial gradients, colored stripes,
 * and high-contrast geometric landmarks so refraction and blur are unmistakable. */
static void draw_backdrop(flux_canvas *c, flux_arena *arena, float W, float H, float t) {
    /* 1. Base deep gradient */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(20, 15, 35, 255)},
            {0.5f, flux_color_rgba_premul(45, 25, 60, 255)},
            {1.0f, flux_color_rgba_premul(15, 30, 45, 255)},
        };
        flux_brush bg =
            flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &bg);
    }

    /* 2. Moving glowing radial orbs */
    {
        float x1 = W * 0.25f + sinf(t * 0.7f) * (W * 0.15f);
        float y1 = H * 0.35f + cosf(t * 0.8f) * (H * 0.15f);
        flux_gradient_stop stops1[2] = {
            {0.0f, flux_color_rgba_premul(255, 90, 50, 220)},
            {1.0f, flux_color_rgba_premul(255, 90, 50, 0)},
        };
        flux_brush b1 = flux_brush_radial_gradient((flux_point){x1, y1}, W * 0.35f, stops1, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b1);

        float x2 = W * 0.75f + cosf(t * 0.6f) * (W * 0.15f);
        float y2 = H * 0.65f + sinf(t * 0.5f) * (H * 0.15f);
        flux_gradient_stop stops2[2] = {
            {0.0f, flux_color_rgba_premul(50, 150, 255, 220)},
            {1.0f, flux_color_rgba_premul(50, 150, 255, 0)},
        };
        flux_brush b2 = flux_brush_radial_gradient((flux_point){x2, y2}, W * 0.35f, stops2, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b2);

        float x3 = W * 0.50f + sinf(t * 1.1f) * (W * 0.20f);
        float y3 = H * 0.50f + cosf(t * 0.9f) * (H * 0.20f);
        flux_gradient_stop stops3[2] = {
            {0.0f, flux_color_rgba_premul(240, 200, 60, 200)},
            {1.0f, flux_color_rgba_premul(240, 200, 60, 0)},
        };
        flux_brush b3 = flux_brush_radial_gradient((flux_point){x3, y3}, W * 0.25f, stops3, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b3);
    }

    /* 3. Diagonal high-contrast zebra stripes to test refraction & edge distortion */
    {
        flux_path *stripes = nullptr;
        (void)flux_path_create(&stripes, arena);
        if (stripes) {
            float spacing = 50.0f;
            for (float x = -H; x < W + H; x += spacing) {
                flux_path_move_to(stripes, x + sinf(t) * 15.0f, 0);
                flux_path_line_to(stripes, x + H + sinf(t) * 15.0f, H);
            }
            flux_brush stripe_brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 45));
            flux_stroke_style st = {.width = 6.0f, .cap = FLUX_CAP_ROUND};
            flux_canvas_stroke_path(c, stripes, &st, &stripe_brush);
        }
    }

    /* 4. Moving geometric rings and discs */
    {
        flux_brush ring_brush = flux_brush_solid(flux_color_rgba_premul(120, 240, 180, 120));
        flux_stroke_style ring_st = {.width = 4.0f};
        flux_path *rings = nullptr;
        (void)flux_path_create(&rings, arena);
        if (rings) {
            float cx = W * 0.5f + cosf(t * 0.4f) * (W * 0.3f);
            float cy = H * 0.5f + sinf(t * 0.4f) * (H * 0.3f);
            flux_path_add_circle(rings, cx, cy, 60.0f);
            flux_path_add_circle(rings, cx, cy, 100.0f);
            flux_canvas_stroke_path(c, rings, &ring_st, &ring_brush);
        }
    }
}

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    gallery_app *app = user;
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

    /* Handle keyboard shortcuts */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == LENS_KEY_ESCAPE || key == 'q' || key == 'Q') {
            iris_window_close();
            return;
        } else if (key == '1') {
            app->view = VIEW_GRID;
        } else if (key == '2') {
            app->view = VIEW_GLASS;
        } else if (key == '3') {
            app->view = VIEW_FROST;
        } else if (key == '4') {
            app->view = VIEW_ACRYLIC;
        } else if (key == '5') {
            app->view = VIEW_MICA;
        } else if (key == ' ' || key == 'p' || key == 'P') {
            app->animating = !app->animating;
        } else if (key == 'd' || key == 'D') {
            app->dark_mode = !app->dark_mode;
            lens_set_theme(ui, app->dark_mode ? lens_theme_dark() : lens_theme_default());
        } else if (key == 'f' || key == 'F') {
            app->inactive_fallback = !app->inactive_fallback;
        }
    }

    if (app->animating)
        iris_request_animation_frame();

    /* Floating control bar at the top with native Lens widgets */
    lens_column_begin(ui, &(lens_layout_opts){.pad = 16, .gap = 12, .cross = LENS_STRETCH});
    lens_row_begin(ui, &(lens_layout_opts){.pad = 8, .gap = 8, .cross = LENS_CENTER});

    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = "[1] 4-Up Grid",
                .variant = (app->view == VIEW_GRID) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->view = VIEW_GRID;
    }
    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = "[2] Liquid Glass",
                .variant = (app->view == VIEW_GLASS) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->view = VIEW_GLASS;
    }
    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = "[3] Frosted Glass",
                .variant = (app->view == VIEW_FROST) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->view = VIEW_FROST;
    }
    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = "[4] Acrylic",
                .variant = (app->view == VIEW_ACRYLIC) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->view = VIEW_ACRYLIC;
    }
    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = "[5] Mica & Mica Alt",
                .variant = (app->view == VIEW_MICA) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->view = VIEW_MICA;
    }

    /* Spacer / separator */
    lens_label(ui, &(lens_label_opts){.text = "|", .size = 14.0f});

    /* Mode toggles */
    if (lens_button(ui,
                    &(lens_button_opts){
                        .label = app->dark_mode ? "[D] Dark (Smoke)" : "[D] Light (Pearl)",
                        .variant = LENS_BUTTON_DEFAULT,
                    })
            .clicked) {
        app->dark_mode = !app->dark_mode;
        lens_set_theme(ui, app->dark_mode ? lens_theme_dark() : lens_theme_default());
    }

    if (lens_button(
            ui,
            &(lens_button_opts){
                .label = app->inactive_fallback ? "[F] Inactive: ON" : "[F] Inactive: OFF",
                .variant = app->inactive_fallback ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
            })
            .clicked) {
        app->inactive_fallback = !app->inactive_fallback;
    }

    if (lens_button(ui,
                    &(lens_button_opts){
                        .label = app->animating ? "[Space] Pause" : "[Space] Resume",
                        .variant = LENS_BUTTON_DEFAULT,
                    })
            .clicked) {
        app->animating = !app->animating;
    }

    lens_close(ui); /* row */

    /* Title / Material caption */
    const char *title_text = "";
    switch (app->view) {
    case VIEW_GRID:
        title_text = "Prism Materials: Liquid Glass (TL), Frosted (TR), Acrylic (BL), Mica (BR)";
        break;
    case VIEW_GLASS:
        title_text = "Liquid Glass: Physical IOR Refraction, Chromatic Dispersion & Specular Rim";
        break;
    case VIEW_FROST:
        title_text = "Frosted Glass: High-Purity Dual-Kawase Blur & Vibrancy Saturation Boost";
        break;
    case VIEW_ACRYLIC:
        title_text = "Acrylic: Dual-Kawase Blur, Luminance Plate Balancing & Procedural Grain";
        break;
    case VIEW_MICA:
        title_text = "Mica & Mica Alt: Wallpaper Sampling, Mineral Dither & Fallback State";
        break;
    }
    lens_label(ui, &(lens_label_opts){.text = title_text, .size = 13.0f});

    lens_close(ui); /* column */
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

    /* Reallocate capture target on resize / scale change */
    if (!app->capture || flux_image_width(app->capture) != W ||
        flux_image_height(app->capture) != H) {
        if (app->capture)
            flux_image_release(app->capture);
        (void)flux_image_create_render_target(device, W, H, FLUX_FORMAT_RGBA8_UNORM, &app->capture);
    }

    /* ===== STEP 1: CAPTURE backdrop into offscreen render target ===== */
    {
        flux_color clear_bg = flux_color_rgba(15, 12, 24, 255);
        (void)flux_canvas_begin(
            canvas, &(flux_canvas_pass_desc){
                        .type = FLUX_TYPE_CANVAS_PASS_DESC,
                        .frame = frame,
                        .attachment = {.kind = FLUX_CANVAS_ATTACHMENT_IMAGE, .image = app->capture},
                        .clear_color = &clear_bg});
        draw_backdrop(canvas, &app->arena, (float)W, (float)H, app->time);
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

    float margin = 28.0f * scale;
    float gap = 22.0f * scale;
    float pad_w = ((float)W - margin * 2.0f - gap) * 0.5f;
    float pad_h = ((float)H - margin * 2.0f - gap) * 0.5f;

    float polarity = app->dark_mode ? 0.0f : 1.0f; /* 0 = smoke, 1 = pearl */

    if (app->view == VIEW_GRID) {
        /* 1. Liquid Glass (Top-Left) */
        {
            prism_liquid_glass_shape shapes[1] = {
                {.bounds = {margin, margin, pad_w, pad_h}, .corner_radius = 16.0f * scale},
            };
            prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
            group.shapes = shapes;
            group.shape_count = 1;
            group.plate_polarity = polarity;
            group.shadow_alpha = 0.25f;
            group.shadow_blur = 16.0f * scale;
            group.shadow_offset_y = 6.0f * scale;

            prism_liquid_glass_desc desc = PRISM_LIQUID_GLASS_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.refraction = 14.0f * scale;
            desc.chromatic_aberration = 1.8f * scale;
            desc.edge_width = 24.0f * scale;
            (void)prism_liquid_glass_filter_apply(app->glass_filter, frame, &desc, &app->glass_out);
        }

        /* 2. Frosted Glass (Top-Right) */
        {
            prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
            group.shape = (prism_frosted_shape){
                .bounds = {margin + pad_w + gap, margin, pad_w, pad_h},
                .corner_radius = 16.0f * scale,
            };
            group.tint_color = app->dark_mode ? 0x1A2540 : 0xEAF2FF;
            group.shadow_alpha = 0.25f;
            group.shadow_blur = 16.0f * scale;
            group.shadow_offset_y = 6.0f * scale;

            prism_frosted_desc desc = PRISM_FROSTED_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.saturation = 1.35f;
            desc.tint_strength = 0.15f;
            (void)prism_frosted_filter_apply(app->frosted_filter, frame, &desc, &app->frost_out);
        }

        /* 3. Acrylic (Bottom-Left) */
        {
            prism_acrylic_group group = PRISM_ACRYLIC_GROUP_INIT;
            group.shape = (prism_acrylic_shape){
                .bounds = {margin, margin + pad_h + gap, pad_w, pad_h},
                .corner_radius = 16.0f * scale,
            };
            group.tint_color = app->dark_mode ? 0x223048 : 0xF0F4F8;
            group.shadow_alpha = 0.25f;
            group.shadow_blur = 16.0f * scale;
            group.shadow_offset_y = 6.0f * scale;

            prism_acrylic_desc desc = PRISM_ACRYLIC_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.luminance_plate = polarity;
            desc.tint_strength = 0.25f;
            desc.noise_intensity = 0.03f;
            desc.border_width = 1.0f * scale;
            desc.border_alpha = 0.18f;
            (void)prism_acrylic_filter_apply(app->acrylic_filter, frame, &desc, &app->acrylic_out);
        }

        /* 4. Mica & Mica Alt (Bottom-Right) */
        {
            float sub_gap = 12.0f * scale;
            float sub_w = (pad_w - sub_gap) * 0.5f;

            prism_mica_group groups[2] = {
                {
                    .shape =
                        {
                            .bounds = {margin + pad_w + gap, margin + pad_h + gap, sub_w, pad_h},
                            .corner_radius = 16.0f * scale,
                        },
                    .shadow_alpha = 0.25f,
                    .shadow_blur = 16.0f * scale,
                    .shadow_offset_y = 6.0f * scale,
                },
                {
                    .shape =
                        {
                            .bounds = {margin + pad_w + gap + sub_w + sub_gap, margin + pad_h + gap,
                                       sub_w, pad_h},
                            .corner_radius = 16.0f * scale,
                        },
                    .shadow_alpha = 0.25f,
                    .shadow_blur = 16.0f * scale,
                    .shadow_offset_y = 6.0f * scale,
                },
            };

            prism_mica_desc desc = PRISM_MICA_DESC_INIT;
            desc.wallpaper = app->capture;
            desc.blurred_wallpaper = blurred;
            desc.groups = groups;
            desc.group_count = 2;
            desc.kind = PRISM_MICA_BASE;
            desc.screen_width = 0.0f;
            desc.screen_height = 0.0f;
            desc.luminosity_plate = polarity;
            desc.tint_color = app->dark_mode ? 0x1E2B3E : 0xEFF3F8;
            desc.fallback_color = app->dark_mode ? 0x202020 : 0xF3F3F3;
            desc.fallback_weight = app->inactive_fallback ? 1.0f : 0.0f;
            (void)prism_mica_filter_apply(app->mica_filter, frame, &desc, &app->mica_out);
        }
    } else {
        /* Fullscreen single-material mode */
        float full_m = 36.0f * scale;
        float full_w = (float)W - full_m * 2.0f;
        float full_h = (float)H - full_m * 2.0f;

        if (app->view == VIEW_GLASS) {
            prism_liquid_glass_shape shapes[1] = {
                {.bounds = {full_m, full_m, full_w, full_h}, .corner_radius = 24.0f * scale},
            };
            prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
            group.shapes = shapes;
            group.shape_count = 1;
            group.plate_polarity = polarity;
            group.shadow_alpha = 0.30f;
            group.shadow_blur = 24.0f * scale;
            group.shadow_offset_y = 10.0f * scale;

            prism_liquid_glass_desc desc = PRISM_LIQUID_GLASS_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.refraction = 20.0f * scale;
            desc.chromatic_aberration = 2.4f * scale;
            desc.edge_width = 30.0f * scale;
            (void)prism_liquid_glass_filter_apply(app->glass_filter, frame, &desc, &app->glass_out);
        } else if (app->view == VIEW_FROST) {
            prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
            group.shape = (prism_frosted_shape){
                .bounds = {full_m, full_m, full_w, full_h},
                .corner_radius = 24.0f * scale,
            };
            group.tint_color = app->dark_mode ? 0x1A2540 : 0xEAF2FF;
            group.shadow_alpha = 0.30f;
            group.shadow_blur = 24.0f * scale;
            group.shadow_offset_y = 10.0f * scale;

            prism_frosted_desc desc = PRISM_FROSTED_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.saturation = 1.45f;
            desc.tint_strength = 0.20f;
            (void)prism_frosted_filter_apply(app->frosted_filter, frame, &desc, &app->frost_out);
        } else if (app->view == VIEW_ACRYLIC) {
            prism_acrylic_group group = PRISM_ACRYLIC_GROUP_INIT;
            group.shape = (prism_acrylic_shape){
                .bounds = {full_m, full_m, full_w, full_h},
                .corner_radius = 24.0f * scale,
            };
            group.tint_color = app->dark_mode ? 0x223048 : 0xF0F4F8;
            group.shadow_alpha = 0.30f;
            group.shadow_blur = 24.0f * scale;
            group.shadow_offset_y = 10.0f * scale;

            prism_acrylic_desc desc = PRISM_ACRYLIC_DESC_INIT;
            desc.input = app->capture;
            desc.blurred_input = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.luminance_plate = polarity;
            desc.tint_strength = 0.30f;
            desc.noise_intensity = 0.03f;
            desc.border_width = 1.5f * scale;
            desc.border_alpha = 0.20f;
            (void)prism_acrylic_filter_apply(app->acrylic_filter, frame, &desc, &app->acrylic_out);
        } else if (app->view == VIEW_MICA) {
            prism_mica_group group = PRISM_MICA_GROUP_INIT;
            group.shape = (prism_mica_shape){
                .bounds = {full_m, full_m, full_w, full_h},
                .corner_radius = 24.0f * scale,
            };
            group.shadow_alpha = 0.30f;
            group.shadow_blur = 24.0f * scale;
            group.shadow_offset_y = 10.0f * scale;

            prism_mica_desc desc = PRISM_MICA_DESC_INIT;
            desc.wallpaper = app->capture;
            desc.blurred_wallpaper = blurred;
            desc.groups = &group;
            desc.group_count = 1;
            desc.kind = PRISM_MICA_BASE;
            desc.screen_width = 0.0f;
            desc.screen_height = 0.0f;
            desc.luminosity_plate = polarity;
            desc.tint_color = app->dark_mode ? 0x1E2B3E : 0xEFF3F8;
            desc.fallback_color = app->dark_mode ? 0x202020 : 0xF3F3F3;
            desc.fallback_weight = app->inactive_fallback ? 1.0f : 0.0f;
            (void)prism_mica_filter_apply(app->mica_filter, frame, &desc, &app->mica_out);
        }
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

int main(void) {
    gallery_app app = {
        .view = VIEW_GRID,
        .animating = true,
        .dark_mode = true,
        .inactive_fallback = false,
        .time = 0.0f,
    };

    printf("=================================================================\n");
    printf(" Optics Prism Material Gallery (Native Iris Showcase)\n");
    printf(" Native Wayland cursor (wp_cursor_shape_v1) & fractional HiDPI\n");
    printf(" Hotkeys: [1..5] Views, [D] Dark/Light, [F] Fallback, [Space] Pause\n");
    printf("=================================================================\n\n");

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
