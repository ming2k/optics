/* id.c — widget identity: FNV-1a hashing over an id stack (ADR-0026). */

#include "../internal.h"

#define LENSI_FNV_OFFSET 1469598103934665603ull
#define LENSI_FNV_PRIME 1099511628211ull

uint64_t lensi_hash(const void *data, size_t len, uint64_t seed) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = seed ? seed : LENSI_FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= LENSI_FNV_PRIME;
    }
    return h;
}

lens_id lensi_id_top(const lens *ui) {
    return ui->id_top ? ui->id_stack[ui->id_top - 1] : LENSI_FNV_OFFSET;
}

/* Length of the visible portion of a label (text before "##"). */
size_t lensi_label_visible_len(const char *label) {
    if (!label)
        return 0;
    const char *hash = strstr(label, "##");
    return hash ? (size_t)(hash - label) : strlen(label);
}

/* Widget id: pure (scope, label) so identity is stable across frames
 * and independent of sibling order (ADR-0026). The full label string —
 * including any "##key" suffix — seeds the hash.
 *
 * An empty label would hash to the raw scope, which is exactly the id of
 * the current container (containers push their id onto the id stack). That
 * makes empty-label widgets collide with their parent container, so they
 * steal its node and leave its real children unarranged. Use a sentinel so
 * empty labels scope under the container instead of replacing it. */
lens_id lensi_gen_widget_id(lens *ui, const char *label) {
    uint64_t scope = lensi_id_top(ui);
    static const char empty_seed[] = "##__flux_empty__";
    const char *seed = label ? label : "";
    size_t len = strlen(seed);
    if (len == 0) {
        seed = empty_seed;
        len = sizeof(empty_seed) - 1;
    }
    lens_id id = lensi_hash(seed, len, scope);
    return id ? id : 1; /* 0 is reserved "no id" */
}

/* Container id: (scope, kind, sibling sequence). Containers carry no
 * label, so a per-parent sequence counter disambiguates siblings and
 * gives each loop iteration's container a distinct scope automatically. */
lens_id lensi_gen_container_id(lens *ui, const char *kind) {
    uint64_t scope = lensi_id_top(ui);
    lens_node *parent = lensi_open_container(ui);
    uint32_t seq = parent ? parent->child_seq++ : 0;
    lens_id id = lensi_hash(kind, strlen(kind), scope);
    id = lensi_hash(&seq, sizeof seq, id);
    return id ? id : 1;
}

/* ---- public id stack ---- */

void lens_push_id(lens *ui, const char *seed) {
    if (ui->id_top >= LENSI_ID_STACK_MAX) {
        ui->overflow = true;
        return;
    }
    const char *s = seed ? seed : "";
    lens_id id = lensi_hash(s, strlen(s), lensi_id_top(ui));
    ui->id_stack[ui->id_top++] = id ? id : 1;
}

void lens_push_id_int(lens *ui, int64_t seed) {
    if (ui->id_top >= LENSI_ID_STACK_MAX) {
        ui->overflow = true;
        return;
    }
    lens_id id = lensi_hash(&seed, sizeof seed, lensi_id_top(ui));
    ui->id_stack[ui->id_top++] = id ? id : 1;
}

void lens_pop_id(lens *ui) {
    if (ui->id_top)
        ui->id_top--;
}

lens_id lens_current_id(const lens *ui, const char *label) {
    uint64_t scope = lensi_id_top(ui);
    const char *seed = label ? label : "";
    lens_id id = lensi_hash(seed, strlen(seed), scope);
    return id ? id : 1;
}
