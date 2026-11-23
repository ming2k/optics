/* drawlist.c — record resolution-deferred draw commands (ADR-0030).
 *
 * Commands address geometry relative to the node box; absolute pixels
 * are resolved at replay, after layout. Commands live in the per-frame
 * arena (ADR-0032). */

#include "../internal.h"
#include <stdio.h>

/* optics_ui__set_overflow lives here (not context.c) because drawlist.c is also
 * compiled directly into test binaries (test_drawlist_hash): the single
 * writer for ui->overflow must link wherever the drawlist does. */
void optics_ui__set_overflow(optics_ui *ui) {
    /* Internal contract: callers pass a live context. The guard is
     * analyzer teeth (clang-analyzer-core.NullDereference), not error
     * handling — every internal call site has a non-NULL ui. */
    if (!ui)
        return;
    ui->overflow = true;
#ifndef NDEBUG
    /* One shot per process: the flag is per frame, the warning is not — a
     * flooded log would bury the signal as effectively as silence. */
    static bool warned;
    if (!warned) {
        warned = true;
        fprintf(stderr,
                "ui: frame arena overflowed; draw calls dropped — virtualize large listings "
                "(see docs/adr/0042)\n");
    }
#endif
}

const char *optics_ui__arena_strndup(optics_ui *ui, const char *s, size_t len) {
    if (!s)
        return nullptr;
    char *copy = optics_gfx_arena_alloc(&ui->arena, len + 1);
    if (!copy) {
        optics_ui__set_overflow(ui);
        return "";
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static uint32_t hash_cmd(const optics_ui_draw_cmd *c) {
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
    /* The borrowed image pointer shapes the pixels too; without it an
     * image-only change left cmd_hash (and so subtree_changed) stale. */
    h = h * 31 + (uint32_t)(uintptr_t)c->image;
    return h;
}

void optics_ui__drawlist_push(optics_ui *ui, optics_ui_node *n, optics_ui_draw_cmd cmd) {
    if (!n)
        return;

    /* Sanitize geometry and numeric properties: a degenerate command (NaN from a
     * division by a zero-height container, or a negative extent from an
     * overshooting spring) must not enter the drawlist. gfx's encoder rejects
     * any non-finite or negative-extent geometry with OPTICS_GFX_ERROR_INVALID_ARGUMENT,
     * and that failure is sticky for the rest of the frame — so one NaN in one
     * widget destroys the entire scene snapshot. Guarding here keeps the failure
     * local: drop a non-finite command and clamp a negative extent/radius to
     * zero. */
    if (!isfinite(cmd.rel.x) || !isfinite(cmd.rel.y) || !isfinite(cmd.rel.w) ||
        !isfinite(cmd.rel.h) || !isfinite(cmd.radius) || !isfinite(cmd.width) ||
        !isfinite(cmd.text_size)) {
        return;
    }
    if (cmd.kind != OPTICS_UI_DRAW_TEXT) {
        if (cmd.rel.w < 0.0f)
            cmd.rel.w = 0.0f;
        if (cmd.rel.h < 0.0f)
            cmd.rel.h = 0.0f;
    }
    if (cmd.radius < 0.0f)
        cmd.radius = 0.0f;
    if (cmd.width < 0.0f)
        cmd.width = 0.0f;

    /* Text commands keep only a pointer, but replay runs after the build
     * phase returns — so a caller's stack buffer (snprintf'd labels,
     * loop-local strings) would dangle. Copy the run into the per-frame
     * arena so any caller lifetime is safe. */
    /* Stamp the context's current text family onto text commands (0 in a
     * widget literal means "inherit"), so replay shapes with the same voice
     * the build-time measure used. */
    if (cmd.kind == OPTICS_UI_DRAW_TEXT && cmd.text_family == 0) {
        cmd.text_family = ui ? ui->text_family : 0;
    }

    if (cmd.kind == OPTICS_UI_DRAW_TEXT && cmd.text)
        cmd.text = optics_ui__arena_strndup(ui, cmd.text, strlen(cmd.text));

    if (!n->cmds || n->cmd_count == n->cmd_cap) {
        /* The frame reset cleared n->cmds while cmd_cap retained last
         * frame's high-water mark, so the first push of a frame sizes the
         * allocation from it in one step — one arena alloc, no doubling
         * chain, no memcpy of previously emitted commands. Growth beyond
         * the retained cap keeps the doubling policy. */
        uint32_t nc = n->cmd_cap ? n->cmd_cap : 8;
        if (n->cmds && n->cmd_count == nc)
            nc = nc * 2;
        optics_ui_draw_cmd *na = optics_gfx_arena_alloc(&ui->arena, nc * sizeof *na);
        if (!na) {
            optics_ui__set_overflow(ui);
            return;
        }
        if (n->cmds && n->cmd_count)
            memcpy(na, n->cmds, n->cmd_count * sizeof *na);
        n->cmds = na;
        n->cmd_cap = nc;
    }
    n->cmds[n->cmd_count++] = cmd;
    n->cmd_hash = n->cmd_hash * 31 + hash_cmd(&cmd);
}
