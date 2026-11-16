/* ui_host.h — null-safe calls into the UI host (iris/host.h).
 *
 * Every backend reaches the UI layer through these and nothing else, so a
 * missing host (`iris_app_opts.ui == nullptr`) or a missing entry behaves the
 * same on Wayland, Win32 and Cocoa: a no-op with a neutral result. */
#ifndef IRIS_UI_HOST_H
#define IRIS_UI_HOST_H

#include <iris/host.h>

static inline bool iris__ui_start(const iris_ui_host *ui, const iris_ui_start_info *info) {
    return !ui || !ui->start || ui->start(ui->user, info);
}
static inline void iris__ui_stop(const iris_ui_host *ui) {
    if (ui && ui->stop)
        ui->stop(ui->user);
}
static inline void iris__ui_destroy(const iris_ui_host *ui) {
    if (ui && ui->destroy)
        ui->destroy(ui->user);
}

/* Build one frame. Without a UI layer the report is neutral: nothing damaged,
 * nothing animating, opaque black background. */
static inline void iris__ui_frame(const iris_ui_host *ui, const iris_input *in,
                                  iris_ui_frame *out) {
    *out = (iris_ui_frame){.clear_color = flux_color_rgba(0x00, 0x00, 0x00, 0xff)};
    if (ui && ui->frame)
        ui->frame(ui->user, in, out);
}

static inline void iris__ui_set_scale(const iris_ui_host *ui, float scale) {
    if (ui && ui->set_scale)
        ui->set_scale(ui->user, scale);
}
static inline void iris__ui_set_color_scheme(const iris_ui_host *ui, iris_color_scheme scheme) {
    if (ui && ui->set_color_scheme)
        ui->set_color_scheme(ui->user, scheme);
}
static inline void iris__ui_set_a11y_prefs(const iris_ui_host *ui, const iris_a11y_prefs *prefs) {
    if (ui && ui->set_a11y_prefs)
        ui->set_a11y_prefs(ui->user, prefs);
}

static inline void iris__ui_paste(const iris_ui_host *ui, const char *utf8, size_t len) {
    if (ui && ui->paste)
        ui->paste(ui->user, utf8, len);
}
static inline void iris__ui_drop(const iris_ui_host *ui, const char *utf8, size_t len,
                                 flux_point pos) {
    if (ui && ui->drop)
        ui->drop(ui->user, utf8, len, pos);
}
static inline void iris__ui_quiesce(const iris_ui_host *ui) {
    if (ui && ui->quiesce)
        ui->quiesce(ui->user);
}

static inline uint32_t iris__ui_damage_rects(const iris_ui_host *ui, flux_rect *rects,
                                             uint32_t capacity) {
    return (ui && ui->damage_rects) ? ui->damage_rects(ui->user, rects, capacity) : 0;
}

/* FLUX_ERROR_UNSUPPORTED without a UI layer: there is no snapshot to make. */
[[nodiscard]] static inline flux_result iris__ui_snapshot_create(const iris_ui_host *ui,
                                                                 iris_ui_snapshot **out) {
    *out = nullptr;
    return (ui && ui->snapshot_create) ? ui->snapshot_create(ui->user, out)
                                       : FLUX_ERROR_UNSUPPORTED;
}
[[nodiscard]] static inline flux_result iris__ui_snapshot_submit(const iris_ui_host *ui,
                                                                 const iris_ui_snapshot *snapshot,
                                                                 flux_canvas *canvas) {
    return (ui && ui->snapshot_submit && snapshot) ? ui->snapshot_submit(snapshot, canvas)
                                                   : FLUX_ERROR_UNSUPPORTED;
}
[[nodiscard]] static inline flux_result iris__ui_snapshot_activate(const iris_ui_host *ui,
                                                                   iris_ui_snapshot *snapshot) {
    return (ui && ui->snapshot_activate && snapshot) ? ui->snapshot_activate(ui->user, snapshot)
                                                     : FLUX_ERROR_UNSUPPORTED;
}
static inline void iris__ui_snapshot_retain(const iris_ui_host *ui, iris_ui_snapshot *snapshot) {
    if (ui && ui->snapshot_retain && snapshot)
        (void)ui->snapshot_retain(snapshot);
}
static inline void iris__ui_snapshot_release(const iris_ui_host *ui, iris_ui_snapshot *snapshot) {
    if (ui && ui->snapshot_release && snapshot)
        ui->snapshot_release(snapshot);
}

#endif /* IRIS_UI_HOST_H */
