/* a11y_stub.c — fallback when libsystemd is not available.
 *
 * All iris_a11y_* entry points report the bridge as unavailable. Callers
 * fall back to the contract-only behaviour: the lens semantic tree still
 * exists and is walked per frame, but no AT-SPI client sees it.
 */

#include "a11y_internal.h"

#ifndef IRIS_HAVE_ATSPI

#include <iris/a11y.h>

IRIS_API int iris_a11y_init(void) {
    return -1;
}
IRIS_API const char *iris_a11y_unique_name(void) {
    return NULL;
}
IRIS_API int iris_a11y_update(lens *ui) {
    (void)ui;
    return -1;
}
IRIS_API void iris_a11y_shutdown(void) { /* no-op */ }

/* Stub: no bridge → nothing to poll/pump. */
int iris_a11y__fd(void) {
    return -1;
}
short iris_a11y__poll_events(void) {
    return 0;
}
void iris_a11y__pump(void) { /* no-op */ }

#endif /* !IRIS_HAVE_ATSPI */
