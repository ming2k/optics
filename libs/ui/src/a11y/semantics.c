/* semantics.c — per-node accessibility record + tree export (ADR-0035).
 *
 * Widgets call lensi_node_semantics during the build phase; the host
 * (an AT-SPI bridge, or a test) reads the result through
 * lens_accessibility_walk after lens_end. lens itself links no
 * assistive-technology library — same host separation as input
 * (ADR-0029) and the text backend (ADR-0033). */

#include "../internal.h"

/* Copy n bytes of s into the per-frame arena as a NUL-terminated string,
 * so a caller's stack/loop-local label survives the post-end walk. */
static const char *arena_strn(optics_ui *ui, const char *s, size_t n) {
    if (!s)
        return nullptr;
    char *c = optics_gfx_arena_alloc(&ui->arena, n + 1);
    if (!c) {
        lensi_set_overflow(ui);
        return "";
    }
    memcpy(c, s, n);
    c[n] = '\0';
    return c;
}

void lensi_node_semantics(optics_ui *ui, optics_ui_node *n, optics_ui_role role, const char *name,
                          const char *value, uint32_t flags) {
    if (!n)
        return;
    n->semantics.role = role;
    n->semantics.name = name ? arena_strn(ui, name, lensi_label_visible_len(name)) : nullptr;
    n->semantics.value = value ? arena_strn(ui, value, strlen(value)) : nullptr;
    n->semantics.flags = flags;
}

void optics_ui_a11y(optics_ui *ui, const optics_ui_a11y_desc *desc) {
    if (!ui || !desc || !ui->last_node)
        return;
    optics_ui_node *n = ui->last_node;
    if (desc->role)
        n->semantics.role = desc->role;
    if (desc->name)
        n->semantics.name = arena_strn(ui, desc->name, strlen(desc->name));
    if (desc->value)
        n->semantics.value = arena_strn(ui, desc->value, strlen(desc->value));
    n->semantics.flags |= desc->flags;
}

void optics_ui_accessibility_walk(const optics_ui *ui, optics_ui_a11y_visit_fn visit, void *user) {
    if (ui)
        optics_ui_snapshot_accessibility_walk(ui->presented_snapshot, visit, user);
}
