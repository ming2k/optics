/*
 * view_grid.c — 4-Up comparative overview exhibiting all four Prism materials side-by-side.
 */

#include "gallery.h"

void gallery_view_grid_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                               const gallery_layout *l) {
    float gap = 18.0f * l->scale;
    float pad_w = (l->avail_w - gap) * 0.5f;
    float pad_h = (l->avail_h - gap) * 0.5f;
    float x0 = l->margin_x;
    float x1 = l->margin_x + pad_w + gap;
    float y0 = l->header_h;
    float y1 = l->header_h + pad_h + gap;

    /* 1. Liquid Glass (Top-Left) */
    {
        prism_liquid_glass_shape shapes[1] = {
            {.bounds = {x0, y0, pad_w, pad_h}, .corner_radius = 16.0f * l->scale},
        };
        prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
        group.shapes = shapes;
        group.shape_count = 1;
        group.plate_polarity = l->polarity;
        group.shadow_alpha = 0.25f;
        group.shadow_blur = 16.0f * l->scale;
        group.shadow_offset_y = 6.0f * l->scale;

        prism_liquid_glass_desc desc = PRISM_LIQUID_GLASS_DESC_INIT;
        desc.input = app->capture;
        desc.blurred_input = blurred;
        desc.groups = &group;
        desc.group_count = 1;
        desc.refraction = 14.0f * l->scale;
        desc.chromatic_aberration = 1.8f * l->scale;
        desc.edge_width = 24.0f * l->scale;
        (void)prism_liquid_glass_filter_apply(app->glass_filter, frame, &desc, &app->glass_out);
    }

    /* 2. Frosted Glass (Top-Right) */
    {
        prism_frosted_group group = PRISM_FROSTED_GROUP_INIT;
        group.shape = (prism_frosted_shape){
            .bounds = {x1, y0, pad_w, pad_h},
            .corner_radius = 16.0f * l->scale,
        };
        group.tint_color = app->dark_mode ? 0x1A2540 : 0xEAF2FF;
        group.shadow_alpha = 0.25f;
        group.shadow_blur = 16.0f * l->scale;
        group.shadow_offset_y = 6.0f * l->scale;

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
            .bounds = {x0, y1, pad_w, pad_h},
            .corner_radius = 16.0f * l->scale,
        };
        group.tint_color = app->dark_mode ? 0x223048 : 0xF0F4F8;
        group.shadow_alpha = 0.25f;
        group.shadow_blur = 16.0f * l->scale;
        group.shadow_offset_y = 6.0f * l->scale;

        prism_acrylic_desc desc = PRISM_ACRYLIC_DESC_INIT;
        desc.input = app->capture;
        desc.blurred_input = blurred;
        desc.groups = &group;
        desc.group_count = 1;
        desc.luminance_plate = l->polarity;
        desc.tint_strength = 0.25f;
        desc.noise_intensity = 0.03f;
        desc.border_width = 1.0f * l->scale;
        desc.border_alpha = 0.18f;
        (void)prism_acrylic_filter_apply(app->acrylic_filter, frame, &desc, &app->acrylic_out);
    }

    /* 4. Mica & Mica Alt (Bottom-Right: split into two side-by-side tiles) */
    {
        float sub_gap = 12.0f * l->scale;
        float sub_w = (pad_w - sub_gap) * 0.5f;

        prism_mica_group groups[2] = {
            {
                .shape =
                    {
                        .bounds = {x1, y1, sub_w, pad_h},
                        .corner_radius = 16.0f * l->scale,
                    },
                .shadow_alpha = 0.25f,
                .shadow_blur = 16.0f * l->scale,
                .shadow_offset_y = 6.0f * l->scale,
            },
            {
                .shape =
                    {
                        .bounds = {x1 + sub_w + sub_gap, y1, sub_w, pad_h},
                        .corner_radius = 16.0f * l->scale,
                    },
                .shadow_alpha = 0.25f,
                .shadow_blur = 16.0f * l->scale,
                .shadow_offset_y = 6.0f * l->scale,
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
        desc.luminosity_plate = l->polarity;
        desc.tint_color = app->dark_mode ? 0x1E2B3E : 0xEFF3F8;
        desc.fallback_color = app->dark_mode ? 0x202020 : 0xF3F3F3;
        desc.fallback_weight = app->inactive_fallback ? 1.0f : 0.0f;
        (void)prism_mica_filter_apply(app->mica_filter, frame, &desc, &app->mica_out);
    }
}
