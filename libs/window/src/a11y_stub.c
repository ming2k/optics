/* a11y_stub.c — fallback when libsystemd is not available.
 *
 * All iris_a11y_* entry points report the bridge as unavailable. Callers
 * fall back to the contract-only behaviour: the lens semantic tree still
 * exists and is walked per frame, but no AT-SPI client sees it.
 */

#include "a11y_internal.h"

#ifndef OPTICS_WINDOW_HAVE_ATSPI

#include <optics/window/a11y.h>

OPTICS_WINDOW_API int optics_window_a11y_init(void) {
    return -1;
}
OPTICS_WINDOW_API bool optics_window_a11y_is_active(void) {
    return false;
}
OPTICS_WINDOW_API int optics_window_a11y_update(const optics_window_ui_host *ui) {
    (void)ui;
    return -1;
}
OPTICS_WINDOW_API void optics_window_a11y_shutdown(void) { /* no-op */ }

/* Stub: no bridge → nothing to poll/pump. */
int optics_window_a11y__fd(void) {
    return -1;
}
short optics_window_a11y__poll_events(void) {
    return 0;
}
void optics_window_a11y__pump(void) { /* no-op */ }

#endif /* !IRIS_HAVE_ATSPI */
