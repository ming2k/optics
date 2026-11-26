/* theme.c — default token sets (reference/api.md). */

#include "../internal.h"

optics_ui_theme optics_ui_theme_dark(void) {
    optics_ui_theme t = {};
    t.size = sizeof(optics_ui_theme);
    /* Sleek dark mode (solid colours) */
    t.color_bg = optics_gfx_color_rgba(0x0e, 0x0e, 0x11, 0xff);     /* deep almost-black */
    t.color_fg = optics_gfx_color_rgba(0xed, 0xed, 0xf0, 0xff);     /* soft off-white */
    t.color_accent = optics_gfx_color_rgba(0x3b, 0x82, 0xf6, 0xff); /* vibrant modern blue */

    t.color_border = optics_gfx_color_rgba(0x2a, 0x2a, 0x30, 0xff); /* solid medium dark grey */
    t.color_hover = optics_gfx_color_rgba(0x1a, 0x1a, 0x20, 0xff);  /* solid dark grey */
    t.color_active =
        optics_gfx_color_rgba(0x24, 0x24, 0x2c, 0xff); /* solid slightly lighter dark grey */

    t.color_disabled = optics_gfx_color_rgba(0x52, 0x52, 0x5b, 0xff); /* zinc 600 */
    t.color_error = optics_gfx_color_rgba(0xef, 0x44, 0x44, 0xff);    /* modern red */

    t.color_surface = optics_gfx_color_rgba(0x15, 0x15, 0x1a, 0xff);
    t.color_surface_sunken = optics_gfx_color_rgba(0x12, 0x12, 0x16, 0xff);
    t.color_surface_elevated = optics_gfx_color_rgba(0x1c, 0x1c, 0x24, 0xff);

    t.padding = 12.0f;
    t.gap = 8.0f;
    t.corner_radius = 6.0f;
    t.border_width = 0.0f;

    t.font = nullptr;
    t.font_size = 14.0f;
    t.icon_size = 0.0f;
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
    t.color_scrollbar_track = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0x08);
    t.color_scrollbar_thumb = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0x24);
    t.color_scrollbar_thumb_hover = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0x40);
    t.color_scrollbar_thumb_active = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0x66);
    t.color_slider_track = t.color_surface_sunken;
    t.color_slider_fill = t.color_accent;
    t.color_slider_knob = t.color_fg;
    t.slider_track_thickness = 6.0f;
    t.slider_knob_size = 14.0f;

    return t;
}

optics_ui_theme optics_ui_theme_default(void) {
    optics_ui_theme t = {};
    t.size = sizeof(optics_ui_theme);
    /* Clean light mode (solid colours) */
    t.color_bg = optics_gfx_color_rgba(0xfa, 0xfa, 0xfb, 0xff);
    t.color_fg = optics_gfx_color_rgba(0x0f, 0x17, 0x2a, 0xff);
    t.color_accent = optics_gfx_color_rgba(0x25, 0x63, 0xeb, 0xff);

    t.color_border = optics_gfx_color_rgba(0xd1, 0xd5, 0xdb, 0xff); /* gray 300 */
    t.color_hover = optics_gfx_color_rgba(0xf3, 0xf4, 0xf6, 0xff);  /* gray 100 */
    t.color_active = optics_gfx_color_rgba(0xe5, 0xe7, 0xeb, 0xff); /* gray 200 */

    t.color_disabled = optics_gfx_color_rgba(0x94, 0xa3, 0xb8, 0xff);
    t.color_error = optics_gfx_color_rgba(0xdc, 0x26, 0x26, 0xff);

    t.color_surface = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0xff);
    t.color_surface_sunken = optics_gfx_color_rgba(0xf1, 0xf3, 0xf5, 0xff);
    t.color_surface_elevated = optics_gfx_color_rgba(0xff, 0xff, 0xff, 0xff);

    t.padding = 12.0f;
    t.gap = 8.0f;
    t.corner_radius = 6.0f;
    t.border_width = 0.0f;

    t.font = nullptr;
    t.font_size = 14.0f;
    t.icon_size = 0.0f;
    t.font_size_title = 28.0f;
    t.font_size_h1 = 22.0f;
    t.font_size_h2 = 18.0f;
    t.font_size_h3 = 15.0f;
    t.font_weight = 400.0f;
    t.font_weight_bold = 600.0f;

    t.scrollbar_width = 6.0f;
    t.scrollbar_radius = 3.0f;
    t.scrollbar_min_thumb_h = 32.0f;
    t.color_scrollbar_track = optics_gfx_color_rgba(0x00, 0x00, 0x00, 0x06);
    t.color_scrollbar_thumb = optics_gfx_color_rgba(0x00, 0x00, 0x00, 0x1c);
    t.color_scrollbar_thumb_hover = optics_gfx_color_rgba(0x00, 0x00, 0x00, 0x33);
    t.color_scrollbar_thumb_active = optics_gfx_color_rgba(0x00, 0x00, 0x00, 0x4a);
    t.color_slider_track = t.color_surface_sunken;
    t.color_slider_fill = t.color_accent;
    t.color_slider_knob = t.color_fg;
    t.slider_track_thickness = 6.0f;
    t.slider_knob_size = 14.0f;

    return t;
}

optics_ui_theme optics_ui_theme_classic(bool dark) {
    optics_ui_theme t = dark ? optics_ui_theme_dark() : optics_ui_theme_default();
    t.border_width = 1.0f;
    return t;
}
