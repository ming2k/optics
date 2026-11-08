/* clipboard.c — host clipboard interface, caret rect, paste queue.
 *
 * The host (the application's platform layer) supplies lens_clipboard at
 * lens_create. lens never links a clipboard or IME library itself —
 * same separation as windowing (ADR-0029) and text (ADR-0033/0011).
 *
 * Paste is asynchronous: a widget (or app) calls lens_request_paste,
 * the host fulfils it later by calling lens_paste, and the next frame's
 * focused text widget drains it via lensi_take_paste. (ADR-0036) */

#include "../internal.h"

/* ---- caret rect (set by the focused text widget; read by the host) ---- */

void lensi_set_caret_rect(lens *ui, flux_rect r) {
    if (!ui)
        return;
    ui->caret_rect = r;
    /* A text widget owns its caret, so lens owns the blink (ADR-0151). */
    ui->caret_lens_owned = r.w > 0.0f && r.h > 0.0f;
}

flux_rect lens_caret_rect(const lens *ui) {
    return ui ? ui->caret_rect : (flux_rect){0, 0, 0, 0};
}

bool lens_caret_visible(const lens *ui) {
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

void lens_set_caret_blink(lens *ui, float period_seconds) {
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

void lensi_set_text_context(lens *ui, const char *utf8, uint32_t len, uint32_t cursor,
                            bool multiline) {
    if (!ui)
        return;
    ui->text_context =
        (lens_text_context){.utf8 = utf8, .len = len, .cursor = cursor, .multiline = multiline};
}

lens_text_context lens_text_context_get(const lens *ui) {
    return ui ? ui->text_context : (lens_text_context){0};
}

/* ---- copy / request-paste / paste (host channel) ---- */

void lens_copy(lens *ui, const char *utf8, size_t len) {
    if (!ui || !utf8 || !len)
        return;
    if (ui->clipboard.set_text)
        ui->clipboard.set_text(utf8, len, ui->clipboard.user);
}

void lens_request_paste(lens *ui) {
    if (!ui)
        return;
    /* Bind the request to the currently focused widget: when the payload
     * arrives (asynchronously, from the host), only that widget may drain
     * it — focus drift between request and fulfilment must not deliver the
     * text elsewhere. 0 = unbound (a host pushing lens_paste without a
     * request keeps the legacy "whoever is focused drains it" path). */
    ui->paste_target = ui->focused_id;
    if (ui->clipboard.request_text)
        ui->clipboard.request_text(ui->clipboard.user);
}

void lens_paste(lens *ui, const char *utf8, size_t len) {
    if (!ui)
        return;
    if (!utf8 || !len) {
        ui->paste_len = 0;
        return;
    }
    if (len > LENSI_PASTE_MAX - 1)
        len = LENSI_PASTE_MAX - 1;
    memcpy(ui->paste_buf, utf8, len);
    ui->paste_buf[len] = '\0';
    ui->paste_len = (uint32_t)len;
    ui->paste_frame = ui->frame; /* delivery window: this frame and the next */
}

const char *lensi_take_paste(lens *ui, uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!ui || !ui->paste_len)
        return NULL;
    /* Drop the payload instead of delivering when the frame window passed
     * (a paste is only fresh for the frame it arrived in and the build
     * right after) or when focus moved away from the requesting widget. */
    bool stale = ui->frame > ui->paste_frame + 1;
    bool wrong_target = ui->paste_target != 0 && ui->paste_target != ui->focused_id;
    if (stale || wrong_target) {
        ui->paste_len = 0;
        return NULL;
    }
    if (out_len)
        *out_len = ui->paste_len;
    ui->paste_len = 0;    /* one-shot; consumed by this caller */
    return ui->paste_buf; /* buffer remains valid until next paste */
}

/* ---- app-owned editing surfaces (public wrappers) ---- */

uint32_t lens_take_paste(lens *ui, char *dst, uint32_t cap) {
    uint32_t len = 0;
    const char *src;
    if (!dst || !cap)
        return 0;
    src = lensi_take_paste(ui, &len);
    if (!src)
        return 0;
    if (len > cap - 1)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

void lens_set_caret_rect(lens *ui, flux_rect r) {
    if (!ui)
        return;
    /* Host-owned caret: the app paints it and owns its clock. Lens records the
     * geometry only, because the platform layer still needs it for the IME
     * candidate window — and explicitly does *not* claim ownership, so this
     * caret contributes no damage and no clock (ADR-0151). */
    ui->caret_rect = r;
    ui->caret_lens_owned = false;
}
