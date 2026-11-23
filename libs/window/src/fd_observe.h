/* fd_observe.h — descriptor and protocol-object accounting (ADR-0129).
 *
 * File descriptors, Wayland proxies and buffers are OPTICS_GFX_SCOPE_PROCESS
 * resources: they are created before any optics_gfx_device exists, they are
 * unbounded by construction, and a leak of one is invisible to every GPU or
 * host counter the stack keeps. Before this header, `app_wayland.c` carried
 * comments admitting the hazard in prose ("or it leaks").
 *
 * The wrappers below are the sanctioned way to open, duplicate or close a
 * descriptor in window. A new raw `close(` / `dup(` / `eventfd(` /
 * `timerfd_create(` call must be paired with a process-scope ledger row in
 * the same change; a leak detector that can be bypassed by the next
 * `#include <unistd.h>` is not a detector.
 *
 * Ownership rule: the wrapper pair is on the *descriptor*, not the call site.
 * Every fd that window opens is opened through optics_window_fd_open_* and closed
 * through optics_window_fd_close_record, so acquire/release are balanced by construction.
 * Fds that window *receives* (wl_keyboard keymap, clipboard pipes) are adopted
 * with optics_window_fd_open_record before their first close.
 */

#ifndef OPTICS_WINDOW_FD_OBSERVE_H
#define OPTICS_WINDOW_FD_OBSERVE_H

#include <optics/gfx/observe.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

/* Owner string shared by every window-side row, so a report answers "who
 * should have closed this" without a code search. */
#define OPTICS_WINDOW_OBSERVE_OWNER "window/wayland"

/* --- raw descriptors ------------------------------------------------- */

static inline void optics_window_fd_open_record(int fd, const char *kind) {
    if (fd >= 0)
        OPTICS_GFX_OBSERVE_ACQUIRE(nullptr, OPTICS_GFX_SCOPE_PROCESS, OPTICS_GFX_LIFETIME_PROCESS,
                                   kind, OPTICS_WINDOW_OBSERVE_OWNER, 0, OPTICS_GFX_SITE_NONE);
}

/* Close and account for a descriptor window owns. */
static inline void optics_window_fd_close_record(int fd, const char *kind) {
    if (fd < 0)
        return;
    OPTICS_GFX_OBSERVE_RELEASE(nullptr, OPTICS_GFX_SCOPE_PROCESS, kind, OPTICS_WINDOW_OBSERVE_OWNER,
                               0);
#if defined(__unix__) || defined(__APPLE__)
    close(fd);
#endif
}

/* Account for a descriptor whose closure happens inside a third-party call
 * (wl_display_disconnect owns and closes its own fd). Separate from
 * optics_window_fd_close_record on purpose: folding the two would make "recorded but
 * still open" and "closed but still recorded" the same code path, which is
 * how a row silently stops matching reality. */
static inline void optics_window_fd_release_record(const char *kind) {
    OPTICS_GFX_OBSERVE_RELEASE(nullptr, OPTICS_GFX_SCOPE_PROCESS, kind, OPTICS_WINDOW_OBSERVE_OWNER,
                               0);
}

/* --- Wayland protocol objects ---------------------------------------- */

/* Wayland objects are not fds, but they are process-scoped, unbounded, and
 * leak in exactly the same way. Counting them by kind ("wl_surface",
 * "wl_buffer", "xdg_toplevel", …) is what turns "the compositor eventually
 * ran out of buffer slots" into a named row. */
static inline void optics_window_wl_acquire(const char *kind) {
    OPTICS_GFX_OBSERVE_ACQUIRE(nullptr, OPTICS_GFX_SCOPE_PROCESS, OPTICS_GFX_LIFETIME_PROCESS, kind,
                               OPTICS_WINDOW_OBSERVE_OWNER, 0, OPTICS_GFX_SITE_NONE);
}

static inline void optics_window_wl_release(const char *kind) {
    OPTICS_GFX_OBSERVE_RELEASE(nullptr, OPTICS_GFX_SCOPE_PROCESS, kind, OPTICS_WINDOW_OBSERVE_OWNER,
                               0);
}

#endif /* OPTICS_WINDOW_FD_OBSERVE_H */
