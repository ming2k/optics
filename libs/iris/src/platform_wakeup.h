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
 *      iris_platform_wakeup_drain() on the loop thread, which runs every
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
 *             call iris_platform_wakeup_drain() before dispatching.
 *
 *   Win32 (app_win32.c, to be implemented):
 *     kick  — PostThreadMessage(loop_thread_id, WM_IRIS_WAKEUP, 0, 0)
 *             where WM_IRIS_WAKEUP is a WM_APP-based message the backend
 *             reserves. PostThreadMessage is thread-safe by design.
 *     drain — the GetMessage/DispatchMessage pump calls
 *             iris_platform_wakeup_drain() when it retrieves WM_IRIS_WAKEUP.
 *
 *   Cocoa (app_cocoa.m, to be implemented):
 *     kick  — CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode,
 *             ^{ iris_platform_wakeup_drain(); }) followed by
 *             CFRunLoopWakeUp(CFRunLoopGetMain()). The block IS the drain;
 *             no extra message handling is needed.
 *
 * Lifecycle: the backend calls iris_platform_wakeup_set_kick(kick, user)
 * on the loop thread before entering its loop and
 * iris_platform_wakeup_set_kick(NULL, NULL) after leaving it (before any
 * teardown a callback could touch). Posts made while no kick is
 * registered fail with -1 and the callback is dropped — subsystems must
 * tolerate this (e.g. a theme change arriving after the window closed).
 */
#ifndef IRIS_PLATFORM_WAKEUP_H
#define IRIS_PLATFORM_WAKEUP_H

/* Callback queued by iris_platform_wakeup_post and run by
 * iris_platform_wakeup_drain on the event-loop thread. */
typedef void (*iris_wakeup_fn)(void *user);

/* Backend-supplied wakeup primitive. MUST be callable from any thread,
 * MUST NOT block, and MUST cause the event loop's wait to return
 * (eventually — coalescing multiple kicks into one wake is fine and
 * expected; the drain empties the whole queue). */
typedef void (*iris_wakeup_kick_fn)(void *user);

/* Register (or clear, with kick == NULL) the active loop's kick. Called by
 * the backend on the event-loop thread. At most one loop is active per
 * process, matching the single-app model of iris_app_run. */
void iris_platform_wakeup_set_kick(iris_wakeup_kick_fn kick, void *user);

/* Enqueue fn(user) and kick the loop. Thread-safe, callable from any
 * thread. Callbacks run in FIFO order on the loop thread.
 * Returns 0 when queued, -1 when no loop is currently registered (the
 * callback is dropped; fn is never run in that case). */
int iris_platform_wakeup_post(iris_wakeup_fn fn, void *user);

/* Run every queued callback, in FIFO order. Event-loop thread only.
 * Callbacks may themselves call iris_platform_wakeup_post; those run on
 * the next drain (or immediately on a later drain in the same wake — the
 * queue is emptied under the lock, then executed outside it). */
void iris_platform_wakeup_drain(void);

#endif /* IRIS_PLATFORM_WAKEUP_H */
