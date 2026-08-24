/* ghost.c — leave-animation snapshots and their replay (ADR-0078).
 *
 * ADR-0038 gave leaving nodes an 8-frame state grace window but no render
 * path; until now the only way to fade a subtree out was for the host to
 * keep building it. This file supplies the missing pixels: at the last
 * live lens_end, a leaving subtree's draw commands and geometry are deep-
 * copied out of the per-frame arena (the arena resets next begin); the
 * host then re-pins the snapshot each frame with lens_set_ghost(id,
 * alpha), and lens_render paints it through the shared command emitter
 * with the ADR-0068 alpha bake.
 *
 * Invariants (ADR-0078):
 *   - bounded: LENSI_GHOST_MAX entries, LENSI_GHOST_MAX_FRAMES lifetime;
 *   - paint-only: no hit-testing, focus, or a11y exposure;
 *   - record-free: snapshots never create canvas display-list records;
 *   - clockless: lifetimes are frame counts, never milliseconds.
 */

#include "../internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- snapshot tree ---------------------------------------------------- */

/* Deep-copy one live node into lensi_alloc memory. Text command payloads
 * are arena-borrowed; each is duplicated so the snapshot survives the
 * reset. Images stay borrowed pointers (the live tree's contract already
 * requires them to outlive lens_render; the ghost extends that to its own
 * expiry — documented at lens_set_ghost). */
static lens_ghost_node *snapshot_node(lens *ui, lens_node *n) {
    lens_ghost_node *g = lensi_alloc(ui, sizeof *g);
    if (!g)
        return NULL;
    memset(g, 0, sizeof *g);
    g->final_rect = n->final_rect;
    g->is_scroll = n->is_scroll;
    g->pad = n->pad;
    g->scroll_gutter = n->scroll_gutter;
    g->place_bounds = n->place_bounds;
    g->has_place_bounds = n->has_place_bounds;

    if (n->cmd_count) {
        g->cmds = lensi_alloc(ui, (size_t)n->cmd_count * sizeof *g->cmds);
        if (!g->cmds) {
            lensi_free(ui, g);
            return NULL;
        }
        /* The count is the full command list unless OOM truncates it at the
         * failing text command (0..i-1 are owned; slot i was not re-alloc'd
         * out of the per-frame arena). Publishing a smaller count
         * unconditionally — as the previous `g->cmd_count = n->cmd_count`
         * rewrite after the loop did in reverse — either frees arena
         * pointers (full count after truncation: heap corruption under
         * OOM) or drops trailing non-text commands (count of text commands
         * only: the ghost fades without them). */
        uint32_t copied = n->cmd_count;
        for (uint32_t i = 0; i < n->cmd_count; i++) {
            g->cmds[i].cmd = n->cmds[i];
            const lens_draw_cmd *c = &n->cmds[i];
            if (c->kind == LENS_DRAW_TEXT && c->text && c->text[0]) {
                size_t len = strlen(c->text) + 1;
                char *copy = lensi_alloc(ui, len);
                if (!copy) {
                    copied = i; /* OOM: truncate; still paint what copied */
                    break;
                }
                memcpy(copy, c->text, len);
                g->cmds[i].cmd.text = copy;
            } else if (c->kind == LENS_DRAW_TEXT) {
                /* Empty/overflow text is not snapshotted: drawlist.c points
                 * overflows at the static "" literal and lensi_free on it
                 * would be an invalid free. Text without ink renders as
                 * nothing; NULL keeps free_snapshot from touching it. */
                g->cmds[i].cmd.text = NULL;
            }
        }
        g->cmd_count = copied;
    }

    lens_ghost_node **tail = &g->first_child;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue; /* band-emitted live; ghosts snapshot the flow walk */
        lens_ghost_node *gc = snapshot_node(ui, c);
        if (!gc)
            break; /* OOM: snapshot what we have */
        *tail = gc;
        tail = &gc->next_sibling;
    }
    return g;
}

static void free_snapshot(lens *ui, lens_ghost_node *g) {
    while (g) {
        lens_ghost_node *next = g->next_sibling;
        free_snapshot(ui, g->first_child);
        if (g->cmds) {
            for (uint32_t i = 0; i < g->cmd_count; i++) {
                const char *t = g->cmds[i].cmd.text;
                if (g->cmds[i].cmd.kind == LENS_DRAW_TEXT && t)
                    lensi_free(ui, (void *)t);
            }
            lensi_free(ui, g->cmds);
        }
        lensi_free(ui, g);
        g = next;
    }
}

/* Total command count of a snapshot subtree (capture eligibility: a
 * subtree with no ink anywhere snapshots to nothing). */
static uint32_t snapshot_cmd_count(const lens_node *n) {
    uint32_t count = n->cmd_count;
    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        count += snapshot_cmd_count(c);
    return count;
}

/* ---- capture (lens_end, after mark_dirty) ----------------------------- */

void lensi_ghost_capture(lens *ui) {
    if (!ui)
        return;
    lens_store *s = &ui->store;
    const uint32_t link_none = UINT32_MAX; /* LENSI_STORE_LINK_NONE (store.c) */

    /* Intent-gated capture: only subtrees the host pinned this frame
     * (lens_set_ghost while the subtree was still being built) are
     * snapshotted. Unconditional capture would repaint every deleted
     * widget for a frame — the exact stale-record artifact the golden
     * record/replay tests forbid (test_child_removal_invalidates_record). */
    uint32_t cursor = s->live_head;
    while (cursor != link_none) {
        uint32_t i = cursor;
        lens_node *n = s->slots[i].node;
        cursor = s->slots[i].next_live;

        if (n->phase != LENS_NODE_LEAVING || n->leaving_frames != 1)
            continue;
        if (!ghost_wants(ui, n->id))
            continue;
        /* Not a root if an enclosing leaving subtree is itself pinned:
         * that outer capture will include this node. */
        bool enclosed = false;
        for (lens_node *p = n->parent; p; p = p->parent) {
            if (p->phase == LENS_NODE_LEAVING && ghost_wants(ui, p->id)) {
                enclosed = true;
                break;
            }
        }
        if (enclosed || ui->ghost_count >= LENSI_GHOST_MAX)
            continue;
        if (n->final_rect.w <= 0.0f || n->final_rect.h <= 0.0f || snapshot_cmd_count(n) == 0u)
            continue;

        lens_ghost *gh = lensi_alloc(ui, sizeof *gh);
        if (!gh)
            continue;
        memset(gh, 0, sizeof *gh);
        gh->root_id = n->id;
        gh->band = n->band;
        gh->frames_left = LENSI_GHOST_MAX_FRAMES;
        /* The arming pin's alpha is the fade's first value; un-refreshed
         * ghosts paint at 1.0 until the host's next pin retints them. */
        gh->alpha = 1.0f;
        for (uint32_t p = 0; p < ui->ghost_pin_count; p++) {
            if (ui->ghost_pins[p] == n->id) {
                gh->alpha = ui->ghost_pin_alphas[p];
                break;
            }
        }
        gh->root = snapshot_node(ui, n);
        if (gh->root)
            ui->ghosts[ui->ghost_count++] = gh;
        else
            lensi_free(ui, gh);
    }
}

/* ---- per-frame bookkeeping -------------------------------------------- */

void lensi_ghost_begin_frame(lens *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->ghost_count; i++)
        ui->ghosts[i]->refreshed_this_frame = false;
    ui->ghost_pin_count = 0; /* pins are per-frame arming, not persistent */
}

/* lens_end tail: expire ghosts the host did not refresh this frame. */
void lensi_ghost_end_frame(lens *ui) {
    if (!ui)
        return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        lens_ghost *gh = ui->ghosts[i];
        if (gh->refreshed_this_frame) {
            gh->frames_left = LENSI_GHOST_MAX_FRAMES;
            ui->ghosts[w++] = gh;
            continue;
        }
        if (gh->frames_left > 0) {
            gh->frames_left--;
            ui->ghosts[w++] = gh;
            continue;
        }
        /* expired: free the snapshot */
        free_snapshot(ui, gh->root);
        lensi_free(ui, gh);
    }
    ui->ghost_count = w;
}

/* ---- public entry ------------------------------------------------------ */

void lensi_ghost_pin(lens *ui, lens_id id, float alpha) {
    if (!ui || !id)
        return;
    if (!(alpha >= 0.0f)) /* NaN guard */
        return;
    if (alpha > 1.0f)
        alpha = 1.0f;

    /* Existing ghost: refresh + retint (the steady fade loop). */
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        if (ui->ghosts[i]->root_id == id) {
            ui->ghosts[i]->alpha = alpha;
            ui->ghosts[i]->refreshed_this_frame = true;
            return;
        }
    }

    /* Not yet a ghost: arm capture. The pin registers intent so the NEXT
     * lens_end snapshots the subtree if it left this frame; an id that
     * never leaves simply clears at the next begin (no cost, no capture).
     * Pins dedupe; a full pin table keeps the earliest (best-practice:
     * arming happens while the subtree is still being built). */
    for (uint32_t i = 0; i < ui->ghost_pin_count; i++) {
        if (ui->ghost_pins[i] == id) {
            ui->ghost_pin_alphas[i] = alpha;
            return;
        }
    }
    if (ui->ghost_pin_count < LENSI_GHOST_MAX) {
        ui->ghost_pins[ui->ghost_pin_count] = id;
        ui->ghost_pin_alphas[ui->ghost_pin_count] = alpha;
        ui->ghost_pin_count++;
    }
    /* A pin past the table is refused like the 17th ghost: structural
     * ceiling, never an error return. */
}

bool ghost_wants(const lens *ui, lens_id id) {
    if (!ui)
        return false;
    for (uint32_t i = 0; i < ui->ghost_pin_count; i++)
        if (ui->ghost_pins[i] == id)
            return true;
    return false;
}

bool lensi_ghost_active(const lens *ui) {
    return ui && ui->ghost_count > 0;
}

/* ---- render ------------------------------------------------------------- */

/* Paint one snapshot node: its own commands at the ghost's alpha, then
 * children through the same scroll-clip discipline as the live walk. */
static void render_ghost_node(lens *ui, flux_canvas *canvas, const lens_ghost_node *g,
                              flux_rect clip, float alpha) {
    flux_rect box = g->final_rect;
    if (box.w <= 0.0f || box.h <= 0.0f)
        return;

    flux_rect command_clip = g->has_place_bounds && g->place_bounds.w >= 0.0f
                                 ? clip
                                 : clip; /* place_bounds recorded; clip comes from band */

    /* Ghosts never record: emit straight through the shared emitter. */
    lensi_emit_commands(ui, canvas, box, command_clip, &g->cmds[0].cmd, g->cmd_count, alpha);

    bool pushed = false;
    if (g->is_scroll && g->first_child) {
        float viewport_w = box.w - 2.0f * g->pad - g->scroll_gutter;
        if (viewport_w < 0.0f)
            viewport_w = 0.0f;
        flux_rect viewport = {box.x + g->pad, box.y + g->pad, viewport_w, box.h - 2.0f * g->pad};
        /* intersect via the emitter's clip stack semantics: children clip
         * through the canvas save/restore the live path uses */
        if (viewport.w > 0.0f && viewport.h > 0.0f) {
            flux_canvas_save(canvas);
            flux_canvas_clip_rect(canvas, viewport);
            pushed = true;
        }
    }
    for (const lens_ghost_node *c = g->first_child; c; c = c->next_sibling)
        render_ghost_node(ui, canvas, c, clip, alpha);
    if (pushed)
        flux_canvas_restore(canvas);
}

void lensi_ghost_render(lens *ui, flux_canvas *canvas) {
    if (!ui || !canvas || ui->ghost_count == 0)
        return;

    /* HiDPI parity with the live tree walk (lensi_render_tree). */
    bool scaled = ui->scale > 0.0f && ui->scale != 1.0f;
    if (scaled) {
        flux_canvas_save(canvas);
        flux_canvas_scale(canvas, ui->scale, ui->scale);
    }
    flux_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};

    /* Band order (ADR-0060): a ghost paints after its band's live nodes;
     * bands ascend BACKDROP → BASE → CHROME → POPUP → TOPMOST. */
    for (lens_band b = LENS_BAND_BACKDROP; b < LENS_BAND_COUNT; b++) {
        for (uint32_t i = 0; i < ui->ghost_count; i++) {
            lens_ghost *gh = ui->ghosts[i];
            if (gh->band != b || !gh->root)
                continue;
            flux_rect clip = gh->root->has_place_bounds ? gh->root->place_bounds : no_clip;
            render_ghost_node(ui, canvas, gh->root, clip, gh->alpha);
        }
    }
    if (scaled)
        flux_canvas_restore(canvas);
}

void lensi_ghost_destroy(lens *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        free_snapshot(ui, ui->ghosts[i]->root);
        lensi_free(ui, ui->ghosts[i]);
    }
    ui->ghost_count = 0;
}
