/* a11y_internal.h — shared header for the AT-SPI bridge sources.
 *
 * Houses the IRIS_HAVE_ATSPI gate so a11y_atspi.c (real bridge) and
 * a11y_stub.c (fallback) stay mutually exclusive, and declares the
 * internal click-synthesis seam consumed by the platform backend.
 */
#ifndef IRIS_A11Y_INTERNAL_H
#define IRIS_A11Y_INTERNAL_H

#include <flux/math.h>
#include <lens/lens.h>
#include <stdbool.h>

/* Drained once per frame by the platform backend (app_wayland.c). If an
 * AT-SPI Action.DoAction call queued a click, fills *out with the widget's
 * centre in UI-space pixels and returns true; the backend synthesizes a
 * press+release there next frame (lens is input-driven). Returns false when
 * nothing is pending. Not part of the public <iris/a11y.h> surface. */
bool iris_a11y__take_pending_click(flux_point *out);

#endif /* IRIS_A11Y_INTERNAL_H */
