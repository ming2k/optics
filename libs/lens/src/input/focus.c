/* focus.c — Tab / Shift+Tab keyboard focus traversal (ADR-0029). */

#include "../internal.h"

/* LENS_KEY_TAB and LENS_MOD_SHIFT are defined in <lens/lens.h>. */

void lensi_focus_tab(lens *ui) {
    if (ui->tab_count == 0)
        return;

    bool tab = false;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        if (ui->input.keys[i].key == LENS_KEY_TAB && ui->input.keys[i].pressed) {
            tab = true;
            break;
        }
    }
    if (!tab)
        return;

    bool back = (ui->input.mods & LENS_MOD_SHIFT) != 0;

    /* The range Tab may cycle over. A modal (ADR-0039) clamps this to the
     * slice recorded while its body was built, trapping focus inside. */
    uint32_t lo = ui->modal_active ? ui->modal_tab_lo : 0;
    uint32_t hi = ui->modal_active ? ui->modal_tab_hi : ui->tab_count;
    uint32_t span = (hi > lo) ? (hi - lo) : 0;
    if (span == 0)
        return;

    /* find the currently focused entry within the active range */
    int cur = -1;
    for (uint32_t i = lo; i < hi; i++) {
        if (ui->tab_order[i] == ui->focused_id) {
            cur = (int)i;
            break;
        }
    }

    int next;
    if (cur < 0) {
        /* focus outside the range (or none): snap to the range edge */
        next = back ? (int)hi - 1 : (int)lo;
    } else {
        next = cur + (back ? -1 : 1);
        if (next < (int)lo)
            next = (int)hi - 1;
        if (next >= (int)hi)
            next = (int)lo;
    }
    ui->focused_id = ui->tab_order[next];
    /* Tab traversal is keyboard navigation: the newly focused widget shows
     * a focus ring (LENS_STATE_FOCUS_VISIBLE, ADR-0058) until a pointer
     * press or a programmatic lens_set_focus revokes the modality. */
    ui->focus_visible = true;
}
