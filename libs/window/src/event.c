/* event.c — the shared pre-fold event seam (ADR-0107).
 *
 * State is main-thread-only by construction: iris_app_run blocks the
 * calling thread and owns the only seam. See event_internal.h for the
 * contract and the pipeline the backends follow.
 */

#include "event_internal.h"

#include <stddef.h>

static optics_window_event_fn g_seam_fn;
static void *g_seam_user;
static uint64_t g_sequence;

void optics_window__event_seam_configure(optics_window_event_fn fn, void *user) {
    g_seam_fn = fn;
    g_seam_user = user;
    g_sequence = 0;
}

void optics_window__event_seam_clear(void) {
    g_seam_fn = nullptr;
    g_seam_user = nullptr;
}

[[nodiscard]] bool optics_window__event_seam_emit(optics_window_event *ev) {
    ev->sequence = ++g_sequence;

    /* No seam: deliver unconditionally at zero cost. */
    if (g_seam_fn == nullptr)
        return true;

    /* Snapshot the undroppable flag before the callback: a host that
     * clears it mid-call cannot thereby make a lifecycle event droppable. */
    bool undroppable = (ev->flags & OPTICS_WINDOW_EVENT_F_UNDROPPABLE) != 0;

    optics_window_event_disposition disposition = g_seam_fn(ev, g_seam_user);

    if (undroppable)
        return true;
    return disposition == OPTICS_WINDOW_EVENT_PASS;
}
