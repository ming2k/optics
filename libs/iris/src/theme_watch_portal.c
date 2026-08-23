/* theme_watch_portal.c — live portal-settings watching via sd-bus.
 *
 * Strategy: open a user sd-bus connection and add a match rule for the
 * org.freedesktop.portal.Settings.SettingChanged signal. A dedicated
 * thread blocks in poll(2) on the bus fd (plus a stop pipe) and pumps the
 * bus; when a signal arrives it does NOT call the user callback directly
 * (wrong thread — the callback contract is main-thread delivery). Instead
 * it posts the change through iris_platform_wakeup_post, and the backend's
 * event loop runs the delivery shim on the iris main thread.
 *
 * This is the ONE portal-settings watcher for the whole library: both the
 * colour-scheme keys (org.freedesktop.appearance / color-scheme, uint32
 * 0/1/2 mapping onto iris_color_scheme) and the accessibility-preference
 * keys (a11y_prefs_internal.h; contrast flag under
 * org.freedesktop.appearance, GNOME's text-scaling-factor /
 * enable-animations under org.gnome.desktop.interface — the portal proxies
 * any gsettings namespace) ride the same connection, thread, and match
 * rule. A second watcher would mean a second D-Bus connection and a second
 * poll thread for the same desktop service; sharing the pump keeps one fd
 * and one delivery path.
 *
 * The a11y keys are handled by topic: key_filter lists the keys this file
 * demuxes; anything else is skipped (message fully drained) so future
 * topics can be added without touching the pump.
 *
 * Build gate: this translation unit is only compiled when libsystemd is
 * available (meson defines IRIS_HAVE_PORTAL_WATCH). When absent, the
 * stubs in theme_watch_stub.c take over and report the feature as
 * unavailable.
 */

#include "theme_watch_internal.h"

#ifdef IRIS_HAVE_PORTAL_WATCH

/* _GNU_SOURCE is provided by the build system (add_project_arguments). */
#include "platform_wakeup.h"

#include "a11y_prefs_internal.h"

#include <iris/theme.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

/* Single watcher per process — sufficient for one running app. */
static sd_bus *g_bus = NULL; /* owned/pumped by g_thread while running */
static pthread_t g_thread;
static bool g_thread_running = false;
static int g_stop_pipe[2] = {-1, -1}; /* written by unwatch, polled by g_thread */

/* Two registrations share the one watcher thread: the host's public slot
 * (iris_color_scheme_watch) and the platform backend's internal slot
 * (iris_theme__watch_backend) — see theme_watch_internal.h for why they
 * must not overwrite each other. Both are touched on the iris main thread
 * only (watch/unwatch/delivery shim), per the public threading contract. */
static iris_color_scheme_changed_fn g_cb = NULL;
static void *g_user = NULL;
static iris_color_scheme_changed_fn g_backend_cb = NULL;
static void *g_backend_user = NULL;

/* Posted through the wakeup seam, freed by the delivery shim. */
typedef struct theme_change {
    iris_color_scheme scheme;
} theme_change;

/* The a11y-preference registrations share the same watcher thread; their
 * slots live here (not in a11y_prefs_portal.c) because the pump, the
 * match rule, and the delivery shim are one mechanism. */
static iris_a11y_prefs_changed_fn g_a11y_cb = NULL;
static void *g_a11y_user = NULL;
static iris_a11y_prefs_changed_fn g_a11y_backend_cb = NULL;
static void *g_a11y_backend_user = NULL;

/* Runs on the iris main thread (via iris_platform_wakeup_drain). */
static void deliver_change(void *user) {
    theme_change *change = user;
    iris_color_scheme scheme = change->scheme;
    free(change);
    if (g_cb)
        g_cb(scheme, g_user);
    if (g_backend_cb)
        g_backend_cb(scheme, g_backend_user);
    if (g_a11y_cb || g_a11y_backend_cb) {
        iris_a11y_prefs prefs = iris_a11y_prefs_query();
        if (g_a11y_cb)
            g_a11y_cb(&prefs, g_a11y_user);
        if (g_a11y_backend_cb)
            g_a11y_backend_cb(&prefs, g_a11y_backend_user);
    }
}

/* SettingChanged signal signature:
 *   s  namespace   e.g. "org.freedesktop.appearance"
 *   s  key         e.g. "color-scheme"
 *   v  value       variant; for color-scheme it is uint32
 *
 * Runs on the watcher thread (inside sd_bus_process). */
static int on_setting_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;

    const char *ns = NULL;
    const char *key = NULL;
    if (sd_bus_message_read(m, "ss", &ns, &key) < 0)
        return 0;

    /* Topics this watcher demuxes. A a11y-relevant key fires the a11y
     * delivery (deliver_change re-queries the full preference set on the
     * main thread, so no variant parsing is needed here); the colour
     * scheme additionally parses its uint32. Everything else is skipped
     * with the variant drained so the message stream stays in sync. */
    bool a11y_key =
        (strcmp(ns, "org.freedesktop.appearance") == 0 && strcmp(key, "contrast") == 0) ||
        (strcmp(ns, "org.gnome.desktop.interface") == 0 &&
         (strcmp(key, "text-scaling-factor") == 0 || strcmp(key, "enable-animations") == 0));
    bool scheme_key =
        strcmp(ns, "org.freedesktop.appearance") == 0 && strcmp(key, "color-scheme") == 0;

    if (!a11y_key && !scheme_key) {
        (void)sd_bus_message_skip(m, "v");
        return 0;
    }

    /* Enter the variant container and read the uint32. */
    uint32_t value = 0;
    if (sd_bus_message_enter_container(m, 'v', "u") < 0)
        return 0;
    if (sd_bus_message_read(m, "u", &value) < 0) {
        (void)sd_bus_message_exit_container(m);
        return 0;
    }
    (void)sd_bus_message_exit_container(m);

    theme_change *change = malloc(sizeof *change);
    if (!change)
        return 0;
    change->scheme = (iris_color_scheme)value;
    /* Hands the change to the backend's event loop, which delivers it on
     * the main thread. -1 (no loop running, e.g. watch used without
     * iris_app_run, or the window already closed) means drop. */
    if (iris_platform_wakeup_post(deliver_change, change) != 0)
        free(change);
    return 0;
}

/* Pump loop: block in poll on the bus fd (mask + timeout as sd-bus
 * requests — sd-bus sockets are level-triggered and MUST be polled with
 * sd_bus_get_events, never a hard-coded POLLIN) and the stop pipe. */
static void *watch_thread_main(void *arg) {
    sd_bus *bus = arg;
    for (;;) {
        for (;;) {
            int rc = sd_bus_process(bus, NULL);
            if (rc < 0)
                return NULL; /* connection lost; unwatch still joins us */
            if (rc == 0)
                break;
        }

        int fd = sd_bus_get_fd(bus);
        int events = sd_bus_get_events(bus);
        if (fd < 0)
            return NULL;

        uint64_t usec = UINT64_MAX;
        (void)sd_bus_get_timeout(bus, &usec);
        int timeout_ms = (usec == UINT64_MAX) ? -1 : (int)(usec / 1000 + 1);

        struct pollfd pfds[2] = {
            {.fd = fd, .events = (short)events},
            {.fd = g_stop_pipe[0], .events = POLLIN},
        };
        if (poll(pfds, 2, timeout_ms) < 0)
            return NULL;
        if (pfds[1].revents & POLLIN)
            return NULL; /* stop requested */
    }
}

/* Start the watcher thread if it is not already running. Returns 0 on
 * success. Main thread only. */
static int watch_start(void) {
    if (g_thread_running)
        return 0;

    int rc = sd_bus_open_user(&g_bus);
    if (rc < 0) {
        g_bus = NULL;
        return -1;
    }

    /* Match only the SettingChanged signal on the portal Settings iface.
     * The path is /org/freedesktop/portal/desktop; sender is the portal. */
    rc = sd_bus_match_signal(g_bus, NULL, "org.freedesktop.portal.Desktop", /* sender            */
                             "/org/freedesktop/portal/desktop",             /* object path       */
                             "org.freedesktop.portal.Settings",             /* interface         */
                             "SettingChanged",                              /* member            */
                             on_setting_changed, NULL);

    if (rc < 0 || pipe(g_stop_pipe) != 0) {
        sd_bus_unref(g_bus);
        g_bus = NULL;
        return -1;
    }

    if (pthread_create(&g_thread, NULL, watch_thread_main, g_bus) != 0) {
        close(g_stop_pipe[0]);
        close(g_stop_pipe[1]);
        g_stop_pipe[0] = g_stop_pipe[1] = -1;
        sd_bus_unref(g_bus);
        g_bus = NULL;
        return -1;
    }

    g_thread_running = true;
    return 0;
}

/* Stop the watcher thread (if running) and release the bus. Returns after
 * the thread has exited. Main thread only. */
static void watch_stop(void) {
    if (!g_thread_running)
        return;

    /* Wake the pump thread out of poll and wait for it to exit before
     * releasing the bus it uses. */
    (void)!write(g_stop_pipe[1], "x", 1);
    pthread_join(g_thread, NULL);
    g_thread_running = false;

    close(g_stop_pipe[0]);
    close(g_stop_pipe[1]);
    g_stop_pipe[0] = g_stop_pipe[1] = -1;

    sd_bus_unref(g_bus);
    g_bus = NULL;
}

IRIS_API int iris_color_scheme_watch(iris_color_scheme_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (watch_start() != 0)
        return -1;
    g_cb = cb;
    g_user = user;
    return 0;
}

IRIS_API void iris_color_scheme_unwatch(void) {
    /* Clear the registration first so a change already sitting in the
     * wakeup queue is dropped by deliver_change instead of calling a dead
     * registration (drain and unwatch both run on the main thread, so this
     * is serialized with delivery). */
    g_cb = NULL;
    g_user = NULL;
    /* The watcher thread keeps running while any other slot is live. */
    if (!g_backend_cb && !g_a11y_cb && !g_a11y_backend_cb)
        watch_stop();
}

int iris_theme__watch_backend(iris_color_scheme_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (watch_start() != 0)
        return -1;
    g_backend_cb = cb;
    g_backend_user = user;
    return 0;
}

void iris_theme__unwatch_backend(void) {
    g_backend_cb = NULL;
    g_backend_user = NULL;
    if (!g_cb)
        watch_stop();
}

/* ---- a11y preferences (same pump, same match rule) ------------------- */

IRIS_API int iris_a11y_prefs_watch(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (watch_start() != 0)
        return -1;
    g_a11y_cb = cb;
    g_a11y_user = user;
    return 0;
}

IRIS_API void iris_a11y_prefs_unwatch(void) {
    g_a11y_cb = NULL;
    g_a11y_user = NULL;
    if (!g_cb && !g_backend_cb && !g_a11y_backend_cb)
        watch_stop();
}

int iris_a11y_prefs__watch_backend(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (watch_start() != 0)
        return -1;
    g_a11y_backend_cb = cb;
    g_a11y_backend_user = user;
    return 0;
}

void iris_a11y_prefs__unwatch_backend(void) {
    g_a11y_backend_cb = NULL;
    g_a11y_backend_user = NULL;
    if (!g_cb && !g_backend_cb && !g_a11y_cb)
        watch_stop();
}

#endif /* IRIS_HAVE_PORTAL_WATCH */
