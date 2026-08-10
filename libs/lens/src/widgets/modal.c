/* modal.c — centered modal dialog: backdrop + focus trap (ADR-0039).
 *
 * Two cooperating placed nodes per open modal (ADR-0060):
 *   - a full-display backdrop (POPUP band, EXACT, non-transient) that dims
 *     the base tree and occludes it via the band-reversed hit-test order,
 *     built just before the content so it paints underneath it, and
 *   - a centered transient content node (POPUP band, CENTERED) carrying
 *     the dialog body.
 * Plus a Tab-range focus trap that clamps keyboard cycling to the slice
 * of tab_order recorded while the modal body was built. */

#include "../internal.h"
#include <stdio.h>

/* Open/close/is_open are thin wrappers over the transient open-set. The
 * dismissal policy is decided at begin-time from lens_modal_opts.pinned. */
void lens_modal_open(lens *ui, const char *id) {
    lens_place_open(ui, id);
}

bool lens_modal_is_open(const lens *ui, const char *id) {
    return lens_place_is_open(ui, id);
}

void lens_modal_close(lens *ui, const char *id) {
    lens_place_close(ui, id);
}

/* Backdrop id must not collide with the content id (which keys the open
 * state). Derive a sibling string. The backdrop is not transient, so it
 * has no open state of its own. */
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
    if (!lensi_place_is_open_id(ui, id))
        return false;

    /* Pin/unpin dismissal for this slot. A reopen after the slot was
     * dropped re-applies the latest policy. */
    int slot = -1;
    for (uint32_t i = 0; i < ui->open_transient_count; i++)
        if (ui->open_transients[i].id == id) {
            slot = (int)i;
            break;
        }
    if (slot >= 0)
        ui->open_transients[slot].dismissable = !opts.pinned;

    float dw = ui->input.display_size.x;
    float dh = ui->input.display_size.y;

    /* --- backdrop: a full-display dim rect in the POPUP band ---------- */
    /* EXACT + non-transient; declared just before the content so it paints
     * (and hit-tests) underneath it within the band. Being in POPUP, it
     * occludes the whole base tree — clicks land on the dim, not on the
     * window behind it. */
    flux_color bg = opts.backdrop ? opts.backdrop : 0x80000000u;
    char bd[80];
    backdrop_label(id_str, bd, sizeof bd);
    if (lens_place_begin(ui, bd,
                         (lens_place_opts){
                             .band = LENS_BAND_POPUP,
                             .mode = LENS_PLACE_EXACT,
                             .rect = {0, 0, dw, dh},
                             .layout = {.bg = bg},
                         })) {
        /* The backdrop carries no body; close it immediately so it is a
         * bare dim rect floating above the base tree. */
        lens_place_end(ui);
    }

    /* --- content: a centered transient node --------------------------- */
    /* The dialog surface follows the cascade (ADR-0061): a scope wrapping
     * the modal retints it like any other widget. */
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    float min_w = opts.min_width > 0 ? opts.min_width : 240.0f;
    if (!lens_place_begin(ui, id_str,
                          (lens_place_opts){
                              .band = LENS_BAND_POPUP,
                              .mode = LENS_PLACE_CENTERED,
                              .transient = true,
                              .layout =
                                  {
                                      .pad = rs.padding * 1.5f,
                                      .gap = rs.gap,
                                      .cross = LENS_STRETCH,
                                      .bg = rs.bg,
                                      .border = rs.border,
                                      .border_width = rs.border_width,
                                      .radius = rs.corner_radius * 1.5f,
                                      .min_width = min_w,
                                  },
                          }))
        return false;

    lens_node *n = ui->last_node;
    if (n)
        lensi_node_semantics(ui, n, LENS_ROLE_DIALOG, opts.title ? opts.title : id_str, NULL, 0);

    /* Push the focus-trap range: the body appends its focusable widgets
     * between now and lens_modal_end. A nested modal suspends the outer
     * range on the trap stack — while the inner modal is open, only the
     * inner range traps (ADR-0039). */
    if (ui->modal_active && ui->modal_trap_depth < LENSI_MODAL_STACK_MAX) {
        ui->modal_trap_lo[ui->modal_trap_depth] = ui->modal_tab_lo;
        ui->modal_trap_hi[ui->modal_trap_depth] = ui->modal_tab_hi;
        ui->modal_trap_depth++;
    }
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
    lens_place_end(ui);
    /* A nested modal's end restores the suspended outer range (ADR-0039).
     * modal_active stays set until lens_end resets it (plus the stack) so
     * lensi_focus_tab, which runs after the build, still sees the trap. */
    if (ui->modal_trap_depth > 0) {
        ui->modal_trap_depth--;
        ui->modal_tab_lo = ui->modal_trap_lo[ui->modal_trap_depth];
        ui->modal_tab_hi = ui->modal_trap_hi[ui->modal_trap_depth];
        ui->modal_active = true;
    }
}
