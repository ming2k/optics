/* skin/image.c — default image skin (ADR-0059): the static lens_image*
 * emit (tinted texture fill + outline atoms) and the interactive
 * image-button emit (blank-tile surface, dark veil + accent border over
 * photography), moved verbatim from widgets/image.c. */

#include "../internal.h"

void lensi_skin_image(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    float w = rec->bounds.w;
    float h = rec->bounds.h;

    if (!rec->content.image_button) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){
                                .kind = LENS_DRAW_IMAGE,
                                .rel = {0, 0, w, h},
                                .color = rec->content.tint,
                                .outline_color = rs->outline_color,
                                .outline_width = rs->outline_width,
                                .image = rec->content.image,
                            });
        return;
    }

    /* Interactive variant (lens_image_button*). */
    bool active = (rec->state & LENS_STATE_ACTIVE) != 0;
    float padding = rs->padding;
    float s = (w < h ? w : h) - 2.0f * padding;
    if (s < 1.0f)
        s = 1.0f;

    float fill = active ? 1.0f : rec->hover_t * 0.6f;
    /* A blank image button still needs the standard surface. For a real
     * image, draw interaction feedback after the texture instead; otherwise
     * the opaque image hides the hover/active treatment completely. The
     * active surface is the neutral tint (resolved bg_pressed), matching
     * lens_icon_button_active (ADR-0061 item 7). */
    if (!rec->content.image && fill > 0.001f) {
        flux_color bg = active ? rs->bg_pressed : lensi_lerp_color(rs->bg, rs->bg_hover, fill);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    if (rec->content.image) {
        float ix = (w - s) * 0.5f;
        float iy = (h - s) * 0.5f;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_IMAGE,
                                            .rel = {ix, iy, s, s},
                                            .color = flux_color_rgba_premul(255, 255, 255, 255),
                                            .image = rec->content.image});

        float feedback = active ? 0.65f + rec->hover_t * 0.35f : rec->hover_t;
        if (feedback > 0.001f) {
            /* A restrained dark veil remains visible over both light and
             * dark photography. The accent outline carries selected/hover
             * state without obscuring the artwork. */
            float veil = active ? 18.0f + rec->hover_t * 34.0f : rec->hover_t * 52.0f;
            uint8_t veil_alpha = (uint8_t)veil;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {ix, iy, s, s},
                                                .color = flux_color_rgba(0, 0, 0, veil_alpha),
                                                .radius = rs->corner_radius});
            lensi_drawlist_push(
                ui, n,
                (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                .rel = {ix, iy, s, s},
                                .color = lensi_lerp_color(rs->border, rs->accent, feedback * 0.75f),
                                .radius = rs->corner_radius,
                                .width = rs->border_width > 1.5f ? rs->border_width : 1.5f});
        }
    }
}
