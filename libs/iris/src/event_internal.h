/* event_internal.h — the shared pre-fold event seam (ADR-0107).
 *
 * This is the single interception point between iris's reification of a
 * native event and the folding of surviving events into the frame's
 * lens_input. Every backend routes each reified `iris_event` through
 * iris__event_seam_emit() before applying it to its accumulator:
 *
 *     iris_event ev = { .type = ..., .flags = ..., .<payload> = ... };
 *     if (!iris__event_seam_emit(&ev))
 *         return;                 // host dropped it
 *     apply(ev);                  // possibly host-rewritten fields
 *
 * iris__event_seam_emit() stamps the monotonic sequence, runs the host
 * callback (if any), and enforces the undroppable rule. The seam state is
 * configured once by iris_app_run() (src/app.c) from cfg->on_event /
 * cfg->user and cleared on return.
 *
 * Thread-affinity (ADR-0043): exactly one app runs per process, on the
 * thread executing iris_app_run. The seam is intentionally not atomic —
 * it is main-thread state, like every other iris callback.
 */
#ifndef IRIS_EVENT_INTERNAL_H
#define IRIS_EVENT_INTERNAL_H

#include <iris/event.h>

/* Install the active host seam and reset the sequence counter. `fn` may
 * be NULL (no interception). Called at the top of iris_app_run. */
void iris__event_seam_configure(iris_event_fn fn, void *user);

/* Release the seam after the backend returns. */
void iris__event_seam_clear(void);

/* Run the seam over one event.
 *
 * Stamps ev->sequence, then (when a callback is installed) invokes it in
 * arrival order. Returns true when the event should be delivered to the
 * accumulator and false when the host dropped it. Events flagged
 * IRIS_EVENT_F_UNDROPPABLE are always delivered, even if the callback
 * returns IRIS_EVENT_DROP; the flag is snapshotted before the callback so
 * a host cannot clear it mid-call. */
[[nodiscard]] bool iris__event_seam_emit(iris_event *ev);

#endif /* IRIS_EVENT_INTERNAL_H */
