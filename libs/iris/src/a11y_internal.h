/* a11y_internal.h — shared header for the AT-SPI bridge sources.
 *
 * Houses the IRIS_HAVE_ATSPI gate so a11y_atspi.c (real bridge) and
 * a11y_stub.c (fallback) stay mutually exclusive, and declares the
 * internal seams consumed by the platform backend.
 *
 * Event-loop integration point (Linux/AT-SPI only): the bridge answers
 * AT-SPI method calls from an sd-bus connection. Backends whose event
 * loop can poll a Unix fd (Wayland) add iris_a11y__fd() to their poll set
 * — with the mask from iris_a11y__poll_events(), never a hard-coded
 * POLLIN, or the level-triggered socket spins the loop — and call
 * iris_a11y__pump() when it is signalled. All three are event-loop-thread
 * only, which keeps every method handler and iris_a11y_update() on one
 * thread: AT-SPI reads of the semantic snapshot stay ordered with the
 * per-frame writes, exactly as the lens contract requires.
 *
 * Win32/Cocoa backends do NOT use this point: their OS accessibility
 * transports (UI Automation / NSAccessibility) are not pollable fds, so
 * those platforms ship their own bridge files satisfying <iris/a11y.h>
 * (until then, a11y_stub.c is compiled and the bridge is inert).
 */
#ifndef IRIS_A11Y_INTERNAL_H
#define IRIS_A11Y_INTERNAL_H

#include <flux/math.h>
#include <lens/lens.h>
#include <stdbool.h>

/* Poll integration for fd-capable backends (see header comment). -1 / 0 /
 * no-op respectively when the bridge is not running. Not part of the
 * public <iris/a11y.h> surface; hidden visibility. */
int iris_a11y__fd(void);
short iris_a11y__poll_events(void);
void iris_a11y__pump(void);

#endif /* IRIS_A11Y_INTERNAL_H */
