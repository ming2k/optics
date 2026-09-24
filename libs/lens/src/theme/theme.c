/* theme.c — default token sets (reference/api.md). */

#include "../internal.h"

lens_material_recipe lens_material_recipe_default(lens_material_kind kind, bool dark) {
    lens_material_recipe r = {0};
    if (dark) {
        switch (kind) {
        case LENS_MATERIAL_MICA:
            r.plate_polarity = 0.0f; /* Smoke */
            r.plate_opacity = 0.65f;
            r.tint = flux_color_rgba(0x1e, 0x2b, 0x3e, 0xff);
            r.tint_opacity = 0.25f;
            r.border_highlight = 0.20f;
            r.shadow_alpha = 0.30f;
            r.shadow_blur = 16.0f;
            r.noise_intensity = 0.02f;
            r.fallback = flux_color_rgba(0x20, 0x20, 0x20, 0xff);
            r.contact_ao = 0.25f;
            r.ambient_fresnel = 0.30f;
            break;
        case LENS_MATERIAL_MICA_ALT:
            r.plate_polarity = 0.0f;
            r.plate_opacity = 0.80f;
            r.tint = flux_color_rgba(0x18, 0x22, 0x32, 0xff);
            r.tint_opacity = 0.40f;
            r.border_highlight = 0.24f;
            r.shadow_alpha = 0.35f;
            r.shadow_blur = 12.0f;
            r.noise_intensity = 0.02f;
            r.fallback = flux_color_rgba(0x18, 0x18, 0x18, 0xff);
            r.contact_ao = 0.25f;
            r.ambient_fresnel = 0.30f;
            break;
        case LENS_MATERIAL_ACRYLIC:
        case LENS_MATERIAL_FROSTED:
            r.plate_polarity = 0.0f;
            r.plate_opacity = 0.85f;
            r.tint = flux_color_rgba(0x22, 0x30, 0x48, 0xff);
            r.tint_opacity = 0.30f;
            r.border_highlight = 0.22f;
            r.shadow_alpha = 0.40f;
            r.shadow_blur = 24.0f;
            r.noise_intensity = 0.03f;
            r.fallback = flux_color_rgba(0x1a, 0x1a, 0x20, 0xff);
            r.contact_ao = 0.30f;
            r.ambient_fresnel = 0.35f;
            break;
        case LENS_MATERIAL_LIQUID_GLASS:
            r.plate_opacity = 0.0f; /* Pure dielectric medium: zero opaque plate */
            r.tint = flux_color_rgba(0xff, 0xff, 0xff, 0xff);
            r.tint_opacity = 0.15f;
            r.border_highlight = 0.60f;
            r.shadow_alpha = 0.35f;
            r.shadow_blur = 16.0f;
            r.noise_intensity = 0.0f;
            r.fallback = flux_color_rgba(0x0e, 0x0e, 0x11, 0xff);
            r.contact_ao = 0.35f;
            r.ambient_fresnel = 0.60f;
            break;
        case LENS_MATERIAL_NONE:
        default:
            break;
        }
    } else {
        switch (kind) {
        case LENS_MATERIAL_MICA:
            r.plate_polarity = 1.0f; /* Pearl */
            r.plate_opacity = 0.65f;
            r.tint = flux_color_rgba(0xef, 0xf3, 0xf8, 0xff);
            r.tint_opacity = 0.20f;
            r.border_highlight = 0.10f;
            r.shadow_alpha = 0.15f;
            r.shadow_blur = 16.0f;
            r.noise_intensity = 0.02f;
            r.fallback = flux_color_rgba(0xf3, 0xf3, 0xf3, 0xff);
            r.contact_ao = 0.35f;
            r.ambient_fresnel = 0.25f;
            break;
        case LENS_MATERIAL_MICA_ALT:
            r.plate_polarity = 1.0f;
            r.plate_opacity = 0.78f;
            r.tint = flux_color_rgba(0xe2, 0xe8, 0xf0, 0xff);
            r.tint_opacity = 0.35f;
            r.border_highlight = 0.12f;
            r.shadow_alpha = 0.18f;
            r.shadow_blur = 12.0f;
            r.noise_intensity = 0.02f;
            r.fallback = flux_color_rgba(0xeb, 0xeb, 0xeb, 0xff);
            r.contact_ao = 0.35f;
            r.ambient_fresnel = 0.25f;
            break;
        case LENS_MATERIAL_ACRYLIC:
        case LENS_MATERIAL_FROSTED:
            r.plate_polarity = 1.0f;
            r.plate_opacity = 0.85f;
            r.tint = flux_color_rgba(0xf0, 0xf4, 0xf8, 0xff);
            r.tint_opacity = 0.25f;
            r.border_highlight = 0.14f;
            r.shadow_alpha = 0.25f;
            r.shadow_blur = 20.0f;
            r.noise_intensity = 0.03f;
            r.fallback = flux_color_rgba(0xf3, 0xf4, 0xf6, 0xff);
            r.contact_ao = 0.40f;
            r.ambient_fresnel = 0.30f;
            break;
        case LENS_MATERIAL_LIQUID_GLASS:
            r.plate_opacity = 0.0f; /* Pure dielectric medium: zero opaque plate */
            r.tint = flux_color_rgba(0xff, 0xff, 0xff, 0xff);
            r.tint_opacity = 0.15f;
            r.border_highlight = 0.50f;
            r.shadow_alpha = 0.25f;
            r.shadow_blur = 16.0f;
            r.noise_intensity = 0.0f;
            r.fallback = flux_color_rgba(0xfa, 0xfa, 0xfb, 0xff);
            r.contact_ao = 0.45f;
            r.ambient_fresnel = 0.50f;
            break;
        case LENS_MATERIAL_NONE:
        default:
            break;
        }
    }
    return r;
}

lens_theme lens_theme_dark(void) {
    lens_theme t = {0};
    t.size = sizeof(lens_theme);
    /* Premium sleek dark mode (Solid colors) */
    t.color_bg = flux_color_rgba(0x0e, 0x0e, 0x11, 0xff);     /* deep almost-black */
    t.color_fg = flux_color_rgba(0xed, 0xed, 0xf0, 0xff);     /* soft off-white */
    t.color_accent = flux_color_rgba(0x3b, 0x82, 0xf6, 0xff); /* vibrant modern blue */

    t.color_border = flux_color_rgba(0x2a, 0x2a, 0x30, 0xff); /* solid medium dark grey */
    t.color_hover = flux_color_rgba(0x1a, 0x1a, 0x20, 0xff);  /* solid dark grey */
    t.color_active = flux_color_rgba(0x24, 0x24, 0x2c, 0xff); /* solid slightly lighter dark grey */

    t.color_disabled = flux_color_rgba(0x52, 0x52, 0x5b, 0xff); /* zinc 600 */
    t.color_error = flux_color_rgba(0xef, 0x44, 0x44, 0xff);    /* modern red */

    t.padding = 12.0f;
    t.gap = 8.0f;
    t.corner_radius = 6.0f;
    t.border_width = 1.0f;

    t.font = NULL;
    t.font_size = 14.0f;
    t.font_size_title = 28.0f;
    t.font_size_h1 = 22.0f;
    t.font_size_h2 = 18.0f;
    t.font_size_h3 = 15.0f;
    t.font_weight = 400.0f;
    t.font_weight_bold = 600.0f;

    /* Scrollbars support alpha correctly, keep them translucent */
    t.scrollbar_width = 6.0f;
    t.scrollbar_radius = 3.0f;
    t.scrollbar_min_thumb_h = 32.0f;
    t.color_scrollbar_track = flux_color_rgba(0xff, 0xff, 0xff, 0x08);
    t.color_scrollbar_thumb = flux_color_rgba(0xff, 0xff, 0xff, 0x24);
    t.color_scrollbar_thumb_hover = flux_color_rgba(0xff, 0xff, 0xff, 0x40);
    t.color_scrollbar_thumb_active = flux_color_rgba(0xff, 0xff, 0xff, 0x66);
    t.color_slider_track = t.color_border;
    t.color_slider_fill = t.color_accent;
    t.color_slider_knob = t.color_fg;
    t.slider_track_thickness = 6.0f;
    t.slider_knob_size = 14.0f;

    /* Surface materials (Smoke plate polarity, subtle specular edge highlight) */
    t.materials.foundation = lens_material_recipe_default(LENS_MATERIAL_MICA, true);
    t.materials.command = lens_material_recipe_default(LENS_MATERIAL_MICA_ALT, true);
    t.materials.floating = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, true);
    t.materials.lens_body = lens_material_recipe_default(LENS_MATERIAL_LIQUID_GLASS, true);

    return t;
}

lens_theme lens_theme_default(void) {
    lens_theme t = {0};
    t.size = sizeof(lens_theme);
    /* Clean, high-end light mode (Solid colors) */
    t.color_bg = flux_color_rgba(0xfa, 0xfa, 0xfb, 0xff);
    t.color_fg = flux_color_rgba(0x0f, 0x17, 0x2a, 0xff);
    t.color_accent = flux_color_rgba(0x25, 0x63, 0xeb, 0xff);

    t.color_border = flux_color_rgba(0xd1, 0xd5, 0xdb, 0xff); /* gray 300 */
    t.color_hover = flux_color_rgba(0xf3, 0xf4, 0xf6, 0xff);  /* gray 100 */
    t.color_active = flux_color_rgba(0xe5, 0xe7, 0xeb, 0xff); /* gray 200 */

    t.color_disabled = flux_color_rgba(0x94, 0xa3, 0xb8, 0xff);
    t.color_error = flux_color_rgba(0xdc, 0x26, 0x26, 0xff);

    t.padding = 12.0f;
    t.gap = 8.0f;
    t.corner_radius = 6.0f;
    t.border_width = 1.0f;

    t.font = NULL;
    t.font_size = 14.0f;
    t.font_size_title = 28.0f;
    t.font_size_h1 = 22.0f;
    t.font_size_h2 = 18.0f;
    t.font_size_h3 = 15.0f;
    t.font_weight = 400.0f;
    t.font_weight_bold = 600.0f;

    t.scrollbar_width = 6.0f;
    t.scrollbar_radius = 3.0f;
    t.scrollbar_min_thumb_h = 32.0f;
    t.color_scrollbar_track = flux_color_rgba(0x00, 0x00, 0x00, 0x06);
    t.color_scrollbar_thumb = flux_color_rgba(0x00, 0x00, 0x00, 0x1c);
    t.color_scrollbar_thumb_hover = flux_color_rgba(0x00, 0x00, 0x00, 0x33);
    t.color_scrollbar_thumb_active = flux_color_rgba(0x00, 0x00, 0x00, 0x4a);
    t.color_slider_track = t.color_border;
    t.color_slider_fill = t.color_accent;
    t.color_slider_knob = t.color_fg;
    t.slider_track_thickness = 6.0f;
    t.slider_knob_size = 14.0f;

    /* Surface materials (Pearl plate polarity, soft diffuse elevation shadow) */
    t.materials.foundation = lens_material_recipe_default(LENS_MATERIAL_MICA, false);
    t.materials.command = lens_material_recipe_default(LENS_MATERIAL_MICA_ALT, false);
    t.materials.floating = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, false);
    t.materials.lens_body = lens_material_recipe_default(LENS_MATERIAL_LIQUID_GLASS, false);

    return t;
}

lens_theme lens_theme_for_material(lens_material_kind kind, bool dark) {
    lens_theme t = dark ? lens_theme_dark() : lens_theme_default();
    lens_material_recipe r = lens_material_recipe_default(kind, dark);

    if (dark) {
        /* Clear solid container fill so the refractive/frosted medium shines through */
        t.color_bg = flux_color_rgba(0x00, 0x00, 0x00, 0x00);
        t.color_fg = flux_color_rgba(0xfa, 0xfa, 0xfc, 0xff);
        t.color_border = flux_color_rgba(0xff, 0xff, 0xff, (uint8_t)(r.border_highlight * 255.0f));
        t.color_hover = flux_color_rgba(0xff, 0xff, 0xff, 0x14);
        t.color_active = flux_color_rgba(0xff, 0xff, 0xff, 0x24);
    } else {
        t.color_bg = flux_color_rgba(0xff, 0xff, 0xff, 0x00);
        t.color_fg = flux_color_rgba(0x0a, 0x0f, 0x1d, 0xff);
        t.color_border =
            flux_color_rgba(0x18, 0x1f, 0x2e, (uint8_t)(r.border_highlight * 255.0f * 0.75f));
        t.color_hover = flux_color_rgba(0x00, 0x00, 0x00, 0x0c);
        t.color_active = flux_color_rgba(0x00, 0x00, 0x00, 0x18);
    }

    return t;
}
