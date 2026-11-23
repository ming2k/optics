/* platform_wakeup.h — cross-thread wakeup seam for platform backends.
 *
 * The one piece of event-loop machinery every backend must provide: a way
 * for ANY thread to wake the backend's blocked event-loop wait and have a
 * callback run on the loop's own thread. Subsystems that listen to the OS
 * on their own thread (the colour-scheme watcher, today; anything
 * future that cannot be expressed as a pollable fd on the loop thread)
 * deliver their results through this seam instead of touching the public
 * API or the backend directly.
 *
 * The mechanism is split in two so each backend only has to supply the
 * primitive its platform actually has:
 *
 *   1. The backend registers a "kick" — a function that is safe to call
 *      from any thread and that makes a thread blocked in the event loop's
 *      wait return. The kick carries no payload.
 *   2. When the loop wakes and sees the kick fired, it calls
 *      optics_window_platform_wakeup_drain() on the loop thread, which runs every
 *      queued callback in FIFO order.
 *
 * The queue itself lives in platform_wakeup.c (shared, lock-protected);
 * backends never touch it directly.
 *
 * Per-backend contract:
 *
 *   Wayland (app_wayland.c):
 *     kick  — write() one byte to a non-blocking eventfd that sits in the
 *             loop's poll(2) set.
 *     drain — when poll reports the eventfd readable, read it empty and
 *             call optics_window_platform_wakeup_drain() before dispatching.
 *
 *   Win32 (app_win32.c, implemented):
 *     kick  — PostThreadMessage(loop_thread_id, WM_WINDOW_WAKEUP, 0, 0)
 *             where WM_WINDOW_WAKEUP is a WM_APP-based message the backend
 *             reserves. PostThreadMessage is thread-safe by design.
 *     drain — the GetMessage/DispatchMessage pump calls
 *             optics_window_platform_wakeup_drain() when it retrieves WM_WINDOW_WAKEUP.
 *
 *   Cocoa (app_cocoa.m, implemented — note the deviation):
 *     kick  — posts an NSEventTypeApplicationDefined event via
 *             [NSApp postEvent:atStart:NO]. Do NOT substitute the literal
 *             CFRunLoopPerformBlock recipe that earlier revisions of this
 *             header prescribed: a PerformBlock scheduled on the main run
 *             loop runs during the wait but does NOT make
 *             nextEventMatchingMask return, so in the fully-idle state
 *             (distantFuture deadline) the frame loop would never wake.
 *             The posted event both wakes the wait and gives the dispatch
 *             site the place to call optics_window_platform_wakeup_drain().
 *
 * Lifecycle: the backend calls optics_window_platform_wakeup_set_kick(kick, user)
 * on the loop thread before entering its loop and
 * optics_window_platform_wakeup_set_kick(NULL, NULL) after leaving it (before any
 * teardown a callback could touch). Posts made while no kick is
 * registered fail with -1 and the callback is dropped — subsystems must
 * tolerate this (e.g. a theme change arriving after the window closed).
 */
#ifndef OPTICS_WINDOW_PLATFORM_WAKEUP_H
#define OPTICS_WINDOW_PLATFORM_WAKEUP_H

/* Callback queued by optics_window_platform_wakeup_post and run by
 * optics_window_platform_wakeup_drain on the event-loop thread. */
typedef void (*optics_window_wakeup_fn)(void *user);

/* Backend-supplied wakeup primitive. MUST be callable from any thread,
 * MUST NOT block, and MUST cause the event loop's wait to return
 * (eventually — coalescing multiple kicks into one wake is fine and
 * expected; the drain empties the whole queue). */
typedef void (*optics_window_wakeup_kick_fn)(void *user);

/* Register (or clear, with kick == NULL) the active loop's kick. Called by
 * the backend on the event-loop thread. At most one loop is active per
 * process, matching the single-app model of optics_window_run. */
void optics_window_platform_wakeup_set_kick(optics_window_wakeup_kick_fn kick, void *user);

/* Enqueue fn(user) and kick the loop. Thread-safe, callable from any
 * thread. Callbacks run in FIFO order on the loop thread.
 * Returns 0 when queued, -1 when no loop is currently registered (the
 * callback is dropped; fn is never run in that case). */
int optics_window_platform_wakeup_post(optics_window_wakeup_fn fn, void *user);

/* Run every queued callback, in FIFO order. Event-loop thread only.
 * Callbacks may themselves call optics_window_platform_wakeup_post; those run on
 * the next drain (or immediately on a later drain in the same wake — the
 * queue is emptied under the lock, then executed outside it). */
void optics_window_platform_wakeup_drain(void);

#endif /* OPTICS_WINDOW_PLATFORM_WAKEUP_H */
