/* drawlist.c — record resolution-deferred draw commands (ADR-0030).
 *
 * Commands address geometry relative to the node box; absolute pixels
 * are resolved at replay, after layout. Commands live in the per-frame
 * arena (ADR-0032). */

#include "../internal.h"

static uint32_t hash_cmd(const lens_draw_cmd *c) {
    uint32_t h = (uint32_t)c->kind;
    h = h * 31 + c->color;
    h = h * 31 + c->outline_color;
    h = h * 31 + (uint32_t)(c->rel.x * 1000.0f);
    h = h * 31 + (uint32_t)(c->rel.y * 1000.0f);
    h = h * 31 + (uint32_t)(c->rel.w * 1000.0f);
    h = h * 31 + (uint32_t)(c->rel.h * 1000.0f);
    h = h * 31 + (uint32_t)(c->radius * 1000.0f);
    h = h * 31 + (uint32_t)(c->width * 1000.0f);
    h = h * 31 + (uint32_t)(c->outline_width * 1000.0f);
    h = h * 31 + (uint32_t)(c->text_size * 1000.0f);
    h = h * 31 + (uint32_t)(c->text_weight * 1000.0f);
    h = h * 31 + (uint32_t)c->text_family;
    if (c->text) {
        const char *p = c->text;
        while (*p)
            h = h * 31 + (unsigned char)*p++;
    }
    h = h * 31 + (uint32_t)c->icon_id;
    /* kind-specific flags (e.g. connected-tab shoulders) and the
     * borrowed image pointer also shape the pixels; without them a
     * flag/weight/image-only change left cmd_hash — and thus
     * subtree_changed — stale. */
    h = h * 31 + c->flags;
    h = h * 31 + (uint32_t)(uintptr_t)c->image;
    return h;
}

void lensi_drawlist_push(lens *ui, lens_node *n, lens_draw_cmd cmd) {
    if (!n)
        return;

    /* Text commands keep only a pointer, but replay runs after the build
     * phase returns — so a caller's stack buffer (snprintf'd labels,
     * loop-local strings) would dangle. Copy the run into the per-frame
     * arena so any caller lifetime is safe. */
    /* Stamp the context's current text family onto text commands (0 in a
     * widget literal means "inherit"), so replay shapes with the same voice
     * the build-time measure used. */
    if (cmd.kind == LENS_DRAW_TEXT && cmd.text_family == 0) {
        cmd.text_family = ui ? ui->text_family : 0;
    }

    if (cmd.kind == LENS_DRAW_TEXT && cmd.text) {
        size_t len = strlen(cmd.text) + 1;
        char *copy = flux_arena_alloc(&ui->arena, len);
        if (copy) {
            memcpy(copy, cmd.text, len);
            cmd.text = copy;
        } else {
            ui->overflow = true;
            cmd.text = "";
        }
    }

    if (n->cmd_count == n->cmd_cap) {
        uint32_t nc = n->cmd_cap ? n->cmd_cap * 2 : 8;
        lens_draw_cmd *na = flux_arena_alloc(&ui->arena, nc * sizeof *na);
        if (!na) {
            ui->overflow = true;
            return;
        }
        if (n->cmds)
            memcpy(na, n->cmds, n->cmd_count * sizeof *na);
        n->cmds = na;
        n->cmd_cap = nc;
    }
    n->cmds[n->cmd_count++] = cmd;
    n->cmd_hash = n->cmd_hash * 31 + hash_cmd(&cmd);
}
