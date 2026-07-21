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
    if (ui)
        ui->caret_rect = r;
}

flux_rect lens_caret_rect(const lens *ui) {
    return ui ? ui->caret_rect : (flux_rect){0, 0, 0, 0};
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
}

const char *lensi_take_paste(lens *ui, uint32_t *out_len) {
    if (!ui || !ui->paste_len) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    if (out_len)
        *out_len = ui->paste_len;
    ui->paste_len = 0;    /* one-shot; consumed by this caller */
    return ui->paste_buf; /* buffer remains valid until next paste */
}
