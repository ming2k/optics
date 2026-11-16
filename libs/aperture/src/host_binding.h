/* host_binding.h — the lens ↔ iris_ui_host binding, without the window.
 *
 * aperture_app_run is this binding handed to iris_app_run. Keeping the binding
 * separate from the run call is what lets tests drive the whole seam
 * headlessly: start, frames, snapshots, deliveries and teardown, with no
 * compositor. */
#ifndef APERTURE_HOST_BINDING_H
#define APERTURE_HOST_BINDING_H

#include <aperture/aperture.h>
#include <iris/host.h>

typedef struct ap_app {
    const aperture_app_opts *opts;
    lens *ui;
    flux_device *device;
    iris_ui_services services;
    bool dark;          /* the scheme the theme currently follows       */
    bool high_contrast; /* the raised-contrast variant is applied       */
    bool app_started;   /* opts->start ran (or was absent) successfully */
} ap_app;

/* Initialise `app` for `opts` and return the host table bound to it. Both
 * must outlive every call made through the table. */
[[nodiscard]] iris_ui_host aperture__bind(ap_app *app, const aperture_app_opts *opts);

#endif /* APERTURE_HOST_BINDING_H */
