/* event_internal.h — the shared pre-fold event seam (ADR-0107).
 *
 * This is the single interception point between window's reification of a
 * native event and the folding of surviving events into the frame's
 * optics_window_input. Every backend routes each reified `optics_window_event` through
 * optics_window__event_seam_emit() before applying it to its accumulator:
 *
 *     optics_window_event ev = { .type = ..., .flags = ..., .<payload> = ... };
 *     if (!optics_window__event_seam_emit(&ev))
 *         return;                 // host dropped it
 *     apply(ev);                  // possibly host-rewritten fields
 *
 * optics_window__event_seam_emit() stamps the monotonic sequence, runs the host
 * callback (if any), and enforces the undroppable rule. The seam state is
 * configured once by optics_window_run() (src/app.c) from cfg->on_event /
 * cfg->user and cleared on return.
 *
 * Thread-affinity (ADR-0043): exactly one app runs per process, on the
 * thread executing optics_window_run. The seam is intentionally not atomic —
 * it is main-thread state, like every other window callback.
 */
#ifndef OPTICS_WINDOW_EVENT_INTERNAL_H
#define OPTICS_WINDOW_EVENT_INTERNAL_H

#include <optics/window/event.h>

/* Install the active host seam and reset the sequence counter. `fn` may
 * be NULL (no interception). Called at the top of optics_window_run. */
void optics_window__event_seam_configure(optics_window_event_fn fn, void *user);

/* Release the seam after the backend returns. */
void optics_window__event_seam_clear(void);

/* Run the seam over one event.
 *
 * Stamps ev->sequence, then (when a callback is installed) invokes it in
 * arrival order. Returns true when the event should be delivered to the
 * accumulator and false when the host dropped it. Events flagged
 * OPTICS_WINDOW_EVENT_F_UNDROPPABLE are always delivered, even if the callback
 * returns OPTICS_WINDOW_EVENT_DROP; the flag is snapshotted before the callback so
 * a host cannot clear it mid-call. */
[[nodiscard]] bool optics_window__event_seam_emit(optics_window_event *ev);

#endif /* OPTICS_WINDOW_EVENT_INTERNAL_H */
