/* clipboard.c — host clipboard interface, caret rect, paste queue.
 *
 * The host (the application's platform layer) supplies optics_ui_clipboard at
 * optics_ui_create. ui never links a clipboard or IME library itself —
 * same separation as windowing (ADR-0029) and text (ADR-0033/0011).
 *
 * Paste is asynchronous: a widget (or app) calls optics_ui_request_paste,
 * the host fulfils it later by calling optics_ui_paste, and the next frame's
 * focused text widget drains it via optics_ui__take_paste. (ADR-0036) */

#include "../internal.h"

/* ---- caret rect (set by the focused text widget; read by the host) ---- */

void optics_ui__set_caret_rect(optics_ui *ui, optics_gfx_rect r) {
    if (!ui)
        return;
    ui->caret_rect = r;
    /* A text widget owns its caret, so ui owns the blink (ADR-0151). */
    ui->caret_ui_owned = r.w > 0.0f && r.h > 0.0f;
}

optics_gfx_rect optics_ui_caret_rect(const optics_ui *ui) {
    return ui ? ui->caret_rect : (optics_gfx_rect){0, 0, 0, 0};
}

bool optics_ui_caret_visible(const optics_ui *ui) {
    if (!ui)
        return false;
    /* A blink would freeze mid-composition: frames are event-driven during
     * IME preedit, and a caret that vanished between them reads as a lost
     * insertion point. */
    if (ui->input.preedit_utf8[0] != '\0')
        return true;
    if (!ui->caret_blink_enabled)
        return true;
    return ui->caret_visible;
}

void optics_ui_set_caret_blink(optics_ui *ui, float period_seconds) {
    if (!ui)
        return;
    if (!(period_seconds > 0.0f) || !isfinite(period_seconds)) {
        ui->caret_blink_enabled = false;
        ui->caret_visible = true;
        return;
    }
    ui->caret_blink_enabled = true;
    ui->caret_blink_period_s = period_seconds;
    /* Keep the phase inside the new period so a mid-cycle period change
     * cannot park the clock past the end and stall the blink. */
    ui->caret_phase_s = fmodf(ui->caret_phase_s, period_seconds);
}

void optics_ui__set_text_context(optics_ui *ui, const char *utf8, uint32_t len, uint32_t cursor,
                                 bool multiline) {
    if (!ui)
        return;
    ui->text_context = (optics_ui_text_context){
        .utf8 = utf8, .len = len, .cursor = cursor, .multiline = multiline};
}

optics_ui_text_context optics_ui_text_context_get(const optics_ui *ui) {
    return ui ? ui->text_context : (optics_ui_text_context){};
}

/* ---- copy / request-paste / paste (host channel) ---- */

void optics_ui_copy(optics_ui *ui, const optics_ui_copy_opts *opts) {
    if (!ui || !opts || !opts->utf8 || !opts->len)
        return;
    if (ui->clipboard.set_text)
        ui->clipboard.set_text(opts->utf8, opts->len, ui->clipboard.user);
}

void optics_ui_request_paste(optics_ui *ui) {
    if (!ui)
        return;
    /* Bind the request to the currently focused widget: when the payload
     * arrives (asynchronously, from the host), only that widget may drain
     * it — focus drift between request and fulfilment must not deliver the
     * text elsewhere. 0 = unbound (a host pushing optics_ui_paste without a
     * request keeps the legacy "whoever is focused drains it" path). */
    ui->paste_target = ui->focused_id;
    if (ui->clipboard.request_text)
        ui->clipboard.request_text(ui->clipboard.user);
}

void optics_ui_paste(optics_ui *ui, const optics_ui_paste_opts *opts) {
    if (!ui)
        return;
    const char *utf8 = opts ? opts->utf8 : nullptr;
    size_t len = opts ? opts->len : 0;
    if (!utf8 || !len) {
        ui->paste_len = 0;
        return;
    }
    if (len > OPTICS_UI_PASTE_MAX - 1)
        len = OPTICS_UI_PASTE_MAX - 1;
    memcpy(ui->paste_buf, utf8, len);
    ui->paste_buf[len] = '\0';
    ui->paste_len = (uint32_t)len;
    ui->paste_frame = ui->frame; /* delivery window: this frame and the next */
}

const char *optics_ui__take_paste(optics_ui *ui, uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!ui || !ui->paste_len)
        return nullptr;
    /* Drop the payload instead of delivering when the frame window passed
     * (a paste is only fresh for the frame it arrived in and the build
     * right after) or when focus moved away from the requesting widget. */
    bool stale = ui->frame > ui->paste_frame + 1;
    bool wrong_target = ui->paste_target != 0 && ui->paste_target != ui->focused_id;
    if (stale || wrong_target) {
        ui->paste_len = 0;
        return nullptr;
    }
    if (out_len)
        *out_len = ui->paste_len;
    ui->paste_len = 0;    /* one-shot; consumed by this caller */
    return ui->paste_buf; /* buffer remains valid until next paste */
}

/* ---- app-owned editing surfaces (public wrappers) ---- */

uint32_t optics_ui_take_paste(optics_ui *ui, const optics_ui_take_paste_opts *opts) {
    uint32_t len = 0;
    const char *src;
    if (!opts || !opts->dst || !opts->cap)
        return 0;
    src = optics_ui__take_paste(ui, &len);
    if (!src)
        return 0;
    if (len > opts->cap - 1)
        len = opts->cap - 1;
    memcpy(opts->dst, src, len);
    opts->dst[len] = '\0';
    return len;
}

void optics_ui_set_caret_rect(optics_ui *ui, optics_gfx_rect r) {
    if (!ui)
        return;
    /* Host-owned caret: the app paints it and owns its clock. ui records the
     * geometry only, because the platform layer still needs it for the IME
     * candidate window — and explicitly does *not* claim ownership, so this
     * caret contributes no damage and no clock (ADR-0151). */
    ui->caret_rect = r;
    ui->caret_ui_owned = false;
}
