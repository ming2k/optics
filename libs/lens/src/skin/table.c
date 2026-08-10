/* skin/table.c — default table skin (ADR-0059): header band, the visible
 * row window (selected / zebra surfaces, per-cell clipped text), and the
 * scrollbar chrome — moved verbatim from widgets/table.c. The
 * virtualization (row window), wheel routing, selection, and the thumb
 * drag state machine are behaviour and stay in the widget. Scrollbar
 * colours are theme tokens with no style slot (the ADR-0059 carve-out),
 * read from the theme like before. */

#include "../internal.h"

void lensi_skin_table(lens *ui, lens_node *n, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;
    const lens_theme *t = &ui->theme;
    float font_size = rs->font_size;
    float padding = rs->padding;
    float body_h = rec->bounds.h - c->header_height;
    if (body_h < 0.0f)
        body_h = 0.0f;
    /* The widget clamps at 32 columns and flags overflow; clamp here too so
     * a hand-built record can never read past the grid arrays. */
    int column_count = c->column_count;
    if (column_count > 32)
        column_count = 32;

    lensi_drawlist_push(ui, n, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH, .rel = {0, 0, 0, 0}});

    /* Header. */
    if (c->header_height > 0.0f) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {0, 0, 0, c->header_height},
                                            .color = rs->bg_hover,
                                            .radius = 0.0f});
        for (int i = 0; i < column_count; i++) {
            const lens_grid_column *col = &c->columns[i];
            if (!col->title)
                continue;
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                                .rel = {col->x + padding,
                                                        (c->header_height - font_size) * 0.5f, 0, 0},
                                                .color = rs->fg,
                                                .text = col->title,
                                                .text_size = font_size,
                                                .text_weight = t->font_weight_bold});
        }
    }

    /* Rows scroll below a fixed header. Give the body its own clip so rows
     * moving out at the top cannot paint across the column titles. */
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH,
                                        .rel = {0, c->header_height, c->view_width, body_h}});

    for (int r = 0; r < c->row_count; r++) {
        const lens_grid_row *row = &c->rows[r];
        bool sel = (row->state & LENS_STATE_SELECTED) != 0;
        if (sel) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, row->y, c->view_width, c->row_height},
                                                .color = rs->bg_pressed,
                                                .radius = 0.0f});
        } else if (row->index % 2 == 1 && c->zebra) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {0, row->y, c->view_width, c->row_height},
                                                .color = lensi_color_alpha(rs->bg_hover, 40),
                                                .radius = 0.0f});
        }
        for (int i = 0; i < column_count; i++) {
            const char *txt = row->cells[i];
            if (!txt || !txt[0])
                continue;
            /* Clip each cell independently. Without this, a long title can
             * paint through artist/album/duration columns even though the
             * table itself is clipped to the viewport. */
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH,
                                                .rel = {c->columns[i].x, row->y, c->columns[i].w,
                                                        c->row_height}});
            lensi_drawlist_push(
                ui, n,
                (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                .rel = {row->cell_x[i],
                                        row->y + (c->row_height - font_size) * 0.5f, 0, 0},
                                .color = sel ? rs->accent : rs->fg,
                                .text = txt,
                                .text_size = font_size});
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP,
                                                .rel = {c->columns[i].x, row->y, c->columns[i].w,
                                                        c->row_height}});
        }
    }

    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP,
                                        .rel = {0, c->header_height, c->view_width, body_h}});

    lensi_drawlist_push(ui, n, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP, .rel = {0, 0, 0, 0}});

    /* Scrollbar (drawn after the clip so it sits above content). */
    if (c->has_scrollbar) {
        if (t->color_scrollbar_track & 0xff000000u) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = c->scrollbar_track,
                                                .color = t->color_scrollbar_track,
                                                .radius = t->scrollbar_radius});
        }
        flux_color tc = (c->scrollbar_state & LENS_STATE_DRAGGED) ? t->color_scrollbar_thumb_active
                        : (c->scrollbar_state & LENS_STATE_HOVERED) ? t->color_scrollbar_thumb_hover
                                                                    : t->color_scrollbar_thumb;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = c->scrollbar_thumb,
                                            .color = tc,
                                            .radius = t->scrollbar_radius});
    }
}
