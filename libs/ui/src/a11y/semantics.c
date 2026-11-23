/* semantics.c — per-node accessibility record + tree export (ADR-0035).
 *
 * Widgets call optics_ui__node_semantics during the build phase; the host
 * (an AT-SPI bridge, or a test) reads the result through
 * optics_ui_accessibility_walk after optics_ui_end. ui itself links no
 * assistive-technology library — same host separation as input
 * (ADR-0029) and the text backend (ADR-0033). */

#include "../internal.h"

void optics_ui__node_semantics(optics_ui *ui, optics_ui_node *n, optics_ui_role role,
                               const char *name, const char *value, uint32_t flags) {
    if (!n)
        return;
    n->semantics.role = role;
    n->semantics.name =
        name ? optics_ui__arena_strndup(ui, name, optics_ui__label_visible_len(name)) : nullptr;
    n->semantics.value = value ? optics_ui__arena_strndup(ui, value, strlen(value)) : nullptr;
    n->semantics.flags = flags;
}

void optics_ui_a11y(optics_ui *ui, const optics_ui_a11y_desc *desc) {
    if (!ui || !desc || !ui->last_node)
        return;
    optics_ui_node *n = ui->last_node;
    if (desc->role)
        n->semantics.role = desc->role;
    if (desc->name)
        n->semantics.name = optics_ui__arena_strndup(ui, desc->name, strlen(desc->name));
    if (desc->value)
        n->semantics.value = optics_ui__arena_strndup(ui, desc->value, strlen(desc->value));
    n->semantics.flags |= desc->flags;
}

void optics_ui_accessibility_walk(const optics_ui *ui, optics_ui_a11y_visit_fn visit, void *user) {
    if (ui)
        optics_ui_snapshot_accessibility_walk(ui->presented_snapshot, visit, user);
}
