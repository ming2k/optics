/* skin/scrollbar.c — scrollbar chrome as a post-layout emission step
 * (ADR-0059).
 *
 * Layout must not emit: scroll_clamp_node (layout/solve.c) only clamps
 * offsets and shifts content. This file is the drawlist-finalize walk that
 * draws scrollbars — it runs from lens_end after the whole tree (placed
 * subtrees included, ADR-0060) has been measured, arranged, and placed, so
 * every rect it reads is final for the frame. The geometry math is the
 * pre-skin code moved verbatim; the walk also persists the thumb geometry
 * that next frame's thumb hit-testing reads (same write timing as before:
 * after arrange, once per frame). */

#include "../internal.h"

/* Draw one scroll container's scrollbar and persist its thumb geometry.
 * Reads solved geometry only: the container's final_rect and its arranged
 * flow children. Nodes that are is_scroll but self-manage scrolling with
 * their own state type (the table's virtualized grid, ADR-0042) have no
 * lens_scroll_state — they draw their own chrome, so this pass leaves
 * them alone. */
static void lensi_skin_scrollbar(lens *ui, lens_node *n) {
    /* Content extent is the union of the FLOW child rects, seeded from the
     * first flow child — ABS children are placed outside the flow
     * (ADR-0060) and never count toward scrollable content (same rule as
     * the clamp pass: seeding from the viewport inflates the union when a
     * large single-frame delta flings every child past the viewport edge). */
    lens_node *seed = NULL;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place != LENS_PLACE_ABS) {
            seed = c;
            break;
        }
    }
    if (!seed)
        return;
    lens_scroll_state *ss = (lens_scroll_state *)lens_node_state(n, sizeof(lens_scroll_state));
    if (!ss)
        return; /* self-scrolling node (table): not scroll.c-managed chrome */

    float min_x = seed->final_rect.x;
    float min_y = seed->final_rect.y;
    float max_x = min_x + seed->final_rect.w;
    float max_y = min_y + seed->final_rect.h;
    for (lens_node *c = seed->next_sibling; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        flux_rect r = c->final_rect;
        if (r.x < min_x)
            min_x = r.x;
        if (r.y < min_y)
            min_y = r.y;
        if (r.x + r.w > max_x)
            max_x = r.x + r.w;
        if (r.y + r.h > max_y)
            max_y = r.y + r.h;
    }
    float content_h = max_y - min_y;
    float viewport_h = n->final_rect.h - 2.0f * n->pad;

    /* Scrollbar geometry (used for both drawing and thumb hit-testing).
     * Every dimension and colour comes from the theme so a consumer can
     * tune the bar to its chrome without touching widget internals. */
    float thumb_h = 0.0f, track_len = 0.0f, scroll_range = 0.0f, thumb_pos = 0.0f;
    bool hovering = ss->hovering;
    bool dragging = ss->dragging;
    if (content_h > viewport_h && viewport_h > 4.0f) {
        const lens_theme *t = &ui->theme;
        float sb_w = t->scrollbar_width;
        float min_thumb = t->scrollbar_min_thumb_h;
        thumb_h = viewport_h * viewport_h / content_h;
        if (thumb_h < min_thumb)
            thumb_h = min_thumb;
        if (thumb_h > viewport_h)
            thumb_h = viewport_h;
        track_len = viewport_h - thumb_h;
        scroll_range = content_h - viewport_h;
        thumb_pos = (scroll_range > 0.0f ? n->scroll_y / scroll_range : 0.0f) * track_len;
        float sb_x = n->final_rect.w - sb_w;
        float sb_y = thumb_pos;
        float radius = t->scrollbar_radius;

        /* track: drawn only when its colour is non-transparent, so a
         * consumer can opt out of the range hint entirely. */
        if (t->color_scrollbar_track & 0xff000000u) {
            lensi_drawlist_push(ui, n,
                                (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                                .rel = {sb_x, 0.0f, sb_w, viewport_h},
                                                .color = t->color_scrollbar_track,
                                                .radius = radius});
        }
        /* thumb: rest / hover / drag colour picked by interaction state */
        flux_color thumb_color = dragging   ? t->color_scrollbar_thumb_active
                                 : hovering ? t->color_scrollbar_thumb_hover
                                            : t->color_scrollbar_thumb;
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                                            .rel = {sb_x, sb_y, sb_w, thumb_h},
                                            .color = thumb_color,
                                            .radius = radius});
    }

    /* persist thumb geometry for next frame's hit-testing (the clamped
     * offsets are layout state; the clamp pass keeps those) */
    ss->thumb_y = thumb_pos;
    ss->thumb_h = thumb_h;
    ss->track_len = track_len;
    ss->scroll_range = scroll_range;
}

static void scrollbar_emit_node(lens *ui, lens_node *n) {
    if (!n)
        return;
    if (n->is_scroll)
        lensi_skin_scrollbar(ui, n);
    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        scrollbar_emit_node(ui, c);
}

void lensi_scrollbars_emit(lens *ui) {
    /* One tree (ADR-0060): the walk reaches placed subtrees — a dropdown's
     * option list — through their parent chain; no separate pass needed. */
    scrollbar_emit_node(ui, ui->root);
}
