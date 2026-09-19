/*
 * view_liquid_glass.c — dedicated Liquid Glass showcase exhibiting the pure physical
 * feature suite: G2 squircle continuous curvature, fluid metaball smooth union,
 * tinted optical absorption, and micro-scale dock pill.
 */

#include "gallery.h"

void gallery_view_glass_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                const gallery_layout *l) {
    (void)app;
    float gap = 20.0f * l->scale;
    float hero_w = l->avail_w * 0.50f - gap * 0.5f;
    float hero_h = l->avail_h;
    float hero_x = l->margin_x;
    float hero_y = l->header_h;

    /* Exhibit 1: Hero Surface (Continuous G2 Superellipse Squircle) */
    prism_liquid_glass_shape hero_shapes[1] = {
        {.bounds = {hero_x, hero_y, hero_w, hero_h}, .corner_radius = 26.0f * l->scale},
    };

    prism_liquid_glass_group hero_group = PRISM_LIQUID_GLASS_GROUP_INIT;
    hero_group.shapes = hero_shapes;
    hero_group.shape_count = 1;
    hero_group.plate_polarity = l->polarity;
    hero_group.shadow_alpha = 0.30f;
    hero_group.shadow_blur = 24.0f * l->scale;
    hero_group.shadow_offset_y = 8.0f * l->scale;
    hero_group.curvature = 1.0f; /* Continuous G2 superellipse squircle */

    /* Exhibit 2: Fluid Metaball Organic Smooth-Union */
    float right_x = hero_x + hero_w + gap;
    float right_w = l->avail_w - hero_w - gap;
    float fusion_h = (l->avail_h - gap) * 0.53f;
    float fusion_y = l->header_h;

    float main_drop_w = right_w - 24.0f * l->scale;
    float main_drop_h = fusion_h - 40.0f * l->scale;
    float orb_w = 110.0f * l->scale;
    float orb_h = 60.0f * l->scale;
    float orb_x = right_x + right_w * 0.40f + sinf(app->time * 1.4f) * (22.0f * l->scale);
    float orb_y = fusion_y + fusion_h - 62.0f * l->scale;

    prism_liquid_glass_shape fusion_shapes[2] = {
        {.bounds = {right_x + 12.0f * l->scale, fusion_y + 8.0f * l->scale, main_drop_w,
                    main_drop_h},
         .corner_radius = 22.0f * l->scale},
        {.bounds = {orb_x, orb_y, orb_w, orb_h}, .corner_radius = 28.0f * l->scale},
    };

    prism_liquid_glass_group fusion_group = PRISM_LIQUID_GLASS_GROUP_INIT;
    fusion_group.shapes = fusion_shapes;
    fusion_group.shape_count = 2;
    fusion_group.blend_radius = 34.0f * l->scale; /* Smooth-union blend radius */
    fusion_group.plate_polarity = l->polarity;
    fusion_group.shadow_alpha = 0.28f;
    fusion_group.shadow_blur = 22.0f * l->scale;
    fusion_group.shadow_offset_y = 7.0f * l->scale;
    fusion_group.curvature = 0.85f;

    /* Exhibit 3: Amber Tinted Optical Glass */
    float bot_y = fusion_y + fusion_h + gap;
    float bot_h = l->avail_h - fusion_h - gap;
    float tint_w = (right_w - gap) * 0.55f;

    prism_liquid_glass_shape tint_shapes[1] = {
        {.bounds = {right_x, bot_y, tint_w, bot_h}, .corner_radius = 20.0f * l->scale},
    };

    prism_liquid_glass_group tint_group = PRISM_LIQUID_GLASS_GROUP_INIT;
    tint_group.shapes = tint_shapes;
    tint_group.shape_count = 1;
    tint_group.plate_polarity = l->polarity;
    tint_group.tint_color = app->glass_amber ? (app->dark_mode ? 0xF5BA42u : 0xD97706u)
                                             : (app->dark_mode ? 0x38BDF8u : 0x0284C7u);
    tint_group.tint_strength = app->glass_amber ? 1.35f : 0.40f;
    tint_group.shadow_alpha = 0.26f;
    tint_group.shadow_blur = 18.0f * l->scale;
    tint_group.shadow_offset_y = 6.0f * l->scale;
    tint_group.curvature = 0.9f;

    /* Exhibit 4: Adaptive Micro-Scale Floating Pill (Dock Handle) */
    float pill_box_x = right_x + tint_w + gap;
    float pill_box_w = right_w - tint_w - gap;
    float handle_w = pill_box_w - 20.0f * l->scale;
    float handle_h = 36.0f * l->scale;
    float handle_x = pill_box_x + 10.0f * l->scale;
    float handle_y = bot_y + (bot_h - handle_h) * 0.5f;

    prism_liquid_glass_shape pill_shapes[1] = {
        {.bounds = {handle_x, handle_y, handle_w, handle_h}, .corner_radius = handle_h * 0.5f},
    };

    prism_liquid_glass_group pill_group = PRISM_LIQUID_GLASS_GROUP_INIT;
    pill_group.shapes = pill_shapes;
    pill_group.shape_count = 1;
    pill_group.plate_polarity = l->polarity;
    pill_group.shadow_alpha = 0.30f;
    pill_group.shadow_blur = 12.0f * l->scale;
    pill_group.shadow_offset_y = 4.0f * l->scale;
    pill_group.curvature = 0.0f; /* Pure Euclidean pill */

    prism_liquid_glass_group groups[4] = {
        hero_group,
        fusion_group,
        tint_group,
        pill_group,
    };

    prism_liquid_glass_desc desc = PRISM_LIQUID_GLASS_DESC_INIT;
    desc.input = app->capture;
    desc.blurred_input = blurred;
    desc.groups = groups;
    desc.group_count = 4;
    desc.refraction = 22.0f * l->scale;
    desc.chromatic_aberration = 2.4f * l->scale;
    desc.edge_width = 24.0f * l->scale;
    desc.rim_light = 0.70f;
    desc.size_reference = 160.0f * l->scale;
    desc.size_scale_min = 0.65f;
    (void)prism_liquid_glass_filter_apply(app->glass_filter, frame, &desc, &app->glass_out);
}
