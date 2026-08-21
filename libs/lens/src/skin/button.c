/* skin/button.c — default button skin (ADR-0059).
 *
 * This is the pre-skin emit section of lens_button moved verbatim: the
 * accent->active body ramp driven by the eased hover/active floats, the
 * WCAG-picked label colour, and the focus border. Replacement skins get
 * the same inputs through the record. */

#include "../internal.h"

/* sRGB channel to linear luminance contribution (WCAG 2.x). */
static float lensi_channel_luminance(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* Relative luminance of an opaque flux_color, WCAG 2.x coefficients. */
static float lensi_relative_luminance(uint32_t rgba) {
    float r = lensi_channel_luminance(((rgba >> 24) & 0xff) / 255.0f);
    float g = lensi_channel_luminance(((rgba >> 16) & 0xff) / 255.0f);
    float b = lensi_channel_luminance(((rgba >> 8) & 0xff) / 255.0f);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* Button label colour for a filled surface: white reads on dark fills but
 * washes out on light accent fills (e.g. a bright positive green), so pick
 * the side with WCAG contrast — white only when it reaches 4.5:1. */
static flux_color lensi_button_text_color(uint32_t bg) {
    float l = lensi_relative_luminance(bg);
    float white_contrast = 1.05f / (l + 0.05f);
    if (white_contrast >= 4.5f)
        return flux_color_rgba(0xff, 0xff, 0xff, 0xff);
    return flux_color_rgba(0x0d, 0x12, 0x10, 0xff);
}

void lensi_skin_button(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    bool disabled = (rec->state & LENS_STATE_DISABLED) != 0;

    /* The body ramp runs accent -> active on the theme path; an instance
     * `bg` replaces the accent end of the ramp (its derived bg_pressed
     * then supplies the pressed end). */
    flux_color body = (rec->style_fields & LENS_STYLE_BG) ? rs->bg : rs->accent;
    flux_color bg =
        disabled ? rs->disabled
                 : lensi_lerp_color(body, rs->bg_pressed,
                                    rec->active_t > rec->hover_t * 0.4f ? rec->active_t
                                                                        : rec->hover_t * 0.4f);

    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = rs->corner_radius});

    /* Vertical centring uses the replay-time convention (negative rel.h —
     * "centre in the final node height", shared with lens_heading and the
     * padded labels): a build-time offset glues the ink to the middle of
     * the MEASURED box, so a button stretched taller than its intrinsic
     * height (a cross-stretched row sibling, an explicit min_height) would
     * show high-riding text. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {rs->padding, 0, -1.0f, -1.0f},
                                        .color = disabled ? rs->fg : lensi_button_text_color(bg),
                                        .text = rec->content.label,
                                        .text_size = rs->font_size});

    if (rec->state & LENS_STATE_FOCUSED)
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                            .rel = {0, 0, 0, 0},
                                            .color = rs->fg,
                                            .radius = rs->corner_radius,
                                            .width = rs->border_width});
}
