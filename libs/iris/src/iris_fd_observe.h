/* iris_fd_observe.h — descriptor and protocol-object accounting (ADR-0129).
 *
 * File descriptors, Wayland proxies and buffers are FLUX_SCOPE_PROCESS
 * resources: they are created before any flux_device exists, they are
 * unbounded by construction, and a leak of one is invisible to every GPU or
 * host counter the stack keeps. Before this header, `app_wayland.c` carried
 * comments admitting the hazard in prose ("or it leaks").
 *
 * The wrappers below are the only sanctioned way to open, duplicate or close
 * a descriptor in iris. `tools/optics check leaks` fails the build on a raw
 * `close(` / `dup(` / `eventfd(` / `timerfd_create(` in libs/iris outside
 * this header, because a leak detector that can be bypassed by the next
 * `#include <unistd.h>` is not a detector.
 *
 * Ownership rule: the wrapper pair is on the *descriptor*, not the call site.
 * Every fd that iris opens is opened through iris_fd_open_* and closed
 * through iris_fd_close, so acquire/release are balanced by construction.
 * Fds that iris *receives* (wl_keyboard keymap, clipboard pipes) are adopted
 * with iris_fd_adopt before their first close.
 */

#ifndef IRIS_FD_OBSERVE_H
#define IRIS_FD_OBSERVE_H

#include <flux/observe.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

/* Owner string shared by every iris-side row, so a report answers "who
 * should have closed this" without a code search. */
#define IRIS_OBSERVE_OWNER "iris/wayland"

/* --- raw descriptors ------------------------------------------------- */

static inline void iris_fd_open_record(int fd, const char *kind) {
    if (fd >= 0)
        FLUX_OBSERVE_ACQUIRE(nullptr, FLUX_SCOPE_PROCESS, FLUX_LIFETIME_PROCESS, kind,
                             IRIS_OBSERVE_OWNER, 0, FLUX_SITE_NONE);
}

/* Close and account for a descriptor iris owns. */
static inline void iris_fd_close_record(int fd, const char *kind) {
    if (fd < 0)
        return;
    FLUX_OBSERVE_RELEASE(nullptr, FLUX_SCOPE_PROCESS, kind, IRIS_OBSERVE_OWNER, 0);
#if defined(__unix__) || defined(__APPLE__)
    close(fd);
#endif
}

/* Account for a descriptor whose closure happens inside a third-party call
 * (wl_display_disconnect owns and closes its own fd). Separate from
 * iris_fd_close_record on purpose: folding the two would make "recorded but
 * still open" and "closed but still recorded" the same code path, which is
 * how a row silently stops matching reality. */
static inline void iris_fd_release_record(const char *kind) {
    FLUX_OBSERVE_RELEASE(nullptr, FLUX_SCOPE_PROCESS, kind, IRIS_OBSERVE_OWNER, 0);
}

/* --- Wayland protocol objects ---------------------------------------- */

/* Wayland objects are not fds, but they are process-scoped, unbounded, and
 * leak in exactly the same way. Counting them by kind ("wl_surface",
 * "wl_buffer", "xdg_toplevel", …) is what turns "the compositor eventually
 * ran out of buffer slots" into a named row. */
static inline void iris_wl_acquire(const char *kind) {
    FLUX_OBSERVE_ACQUIRE(nullptr, FLUX_SCOPE_PROCESS, FLUX_LIFETIME_PROCESS, kind,
                         IRIS_OBSERVE_OWNER, 0, FLUX_SITE_NONE);
}

static inline void iris_wl_release(const char *kind) {
    FLUX_OBSERVE_RELEASE(nullptr, FLUX_SCOPE_PROCESS, kind, IRIS_OBSERVE_OWNER, 0);
}

#endif /* IRIS_FD_OBSERVE_H */
