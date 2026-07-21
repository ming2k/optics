/* modal.c — centered modal dialog: backdrop + focus trap (ADR-0039).
 *
 * Two cooperating floating layers per open modal:
 *   - a full-display backdrop panel (non-dismissible, dims + eclipses the
 *     base tree via the existing floating-layer eclipse check), and
 *   - a centered content overlay carrying the dialog body.
 * Plus a Tab-range focus trap that clamps keyboard cycling to the slice
 * of tab_order recorded while the modal body was built. */

#include "../internal.h"
#include <stdio.h>

/* Open/close/is_open are thin wrappers over the overlay open-set. The
 * dismissability is decided at begin-time from lens_modal_opts. */
void lens_modal_open(lens *ui, const char *id) {
    lens_overlay_open(ui, id);
}

bool lens_modal_is_open(const lens *ui, const char *id) {
    return lens_overlay_is_open(ui, id);
}

void lens_modal_close(lens *ui, const char *id) {
    lens_overlay_close(ui, id);
}

/* Backdrop id must not collide with the content id (which keys the open
 * state). Derive a sibling string. The backdrop is a panel layer, never
 * in the open-set, so it has no open state of its own. */
static void backdrop_label(const char *id, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "%s##bd", id);
    if (n < 0 || (size_t)n >= cap) {
        size_t l = id ? strlen(id) : 0;
        if (l > cap - 6)
            l = cap - 6;
        memcpy(buf, id, l);
        memcpy(buf + l, "##bd", 5);
    }
}

bool lens_modal_begin(lens *ui, const char *id_str, lens_modal_opts opts) {
    if (!ui || !id_str)
        return false;
    lens_id id = lensi_gen_widget_id(ui, id_str);
    if (!lensi_overlay_is_open_id(ui, id))
        return false;

    /* Pin/unpin dismissal for this slot. A reopen after the slot was
     * dropped re-applies the latest policy. */
    int slot = -1;
    for (uint32_t i = 0; i < ui->open_overlay_count; i++)
        if (ui->open_overlays[i].id == id) {
            slot = (int)i;
            break;
        }
    if (slot >= 0)
        ui->open_overlays[slot].dismissable = opts.dismissable;

    float dw = ui->input.display_size.x;
    float dh = ui->input.display_size.y;

    /* --- backdrop: a persistent panel covering the whole display ------- */
    flux_color bg = opts.backdrop ? opts.backdrop : 0x80000000u;
    char bd[80];
    backdrop_label(id_str, bd, sizeof bd);
    if (lens_layer_begin(ui, bd, (flux_rect){0, 0, dw, dh}, (lens_overlay_opts){.bg = bg})) {
        /* The backdrop carries no body; close it immediately so it is a
         * bare dim rect floating above the base tree. */
        lens_layer_end(ui);
    }

    /* --- content: a centered overlay ----------------------------------- */
    float min_w = opts.min_width > 0 ? opts.min_width : 240.0f;
    flux_rect anchor = {0, 0, min_w, 0}; /* sized by min_width; centered by flag */
    lens_overlay_opts ov = {
        .pad = ui->theme.padding * 1.5f,
        .gap = ui->theme.gap,
        .cross = LENS_STRETCH,
        .bg = ui->theme.color_bg,
        .border = ui->theme.color_border,
        .border_width = ui->theme.border_width,
        .radius = ui->theme.corner_radius * 1.5f,
        .min_width = min_w,
    };
    if (!lens_overlay_begin(ui, id_str, anchor, ov))
        return false;

    /* Mark the content node for centered placement. */
    lens_node *n = ui->last_node;
    if (n) {
        n->is_centered = true;
        lensi_node_semantics(ui, n, LENS_ROLE_DIALOG, opts.title ? opts.title : id_str, NULL, 0);
    }

    /* Start the focus-trap range at the current tab-order length; the body
     * appends its focusable widgets between now and lens_modal_end. */
    ui->modal_active = true;
    ui->modal_tab_lo = ui->tab_count;
    ui->modal_tab_hi = ui->tab_count;

    /* Optional title row. */
    if (opts.title)
        lens_title(ui, opts.title);
    return true;
}

void lens_modal_end(lens *ui) {
    if (!ui)
        return;
    /* Close the focus-trap range: widgets built inside the body now form
     * a contiguous slice of tab_order. */
    if (ui->modal_active)
        ui->modal_tab_hi = ui->tab_count;
    lens_overlay_end(ui);
    /* modal_active stays set until lens_end resets it so lensi_focus_tab,
     * which runs after the build, still sees the trap. It is cleared at
     * lens_end (context.c) along with the per-frame counters. */
}
