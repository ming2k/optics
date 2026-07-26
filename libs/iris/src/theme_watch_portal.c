/* theme_watch_portal.c — live system colour-scheme watching via sd-bus.
 *
 * Strategy: open a system sd-bus connection, add a match rule for the
 * org.freedesktop.portal.Settings.SettingChanged signal filtered to the
 * "org.freedesktop.appearance" namespace + "color-scheme" key, then let
 * the caller poll our fd and pump messages into the callback when the fd
 * is readable.
 *
 * The portal encodes the colour scheme as a uint32 variant:
 *   0  no preference
 *   1  prefer dark
 *   2  prefer light
 *
 * which maps directly onto iris_color_scheme.
 *
 * Build gate: this translation unit is only compiled when libsystemd is
 * available (meson defines IRIS_HAVE_PORTAL_WATCH). When absent, the
 * stubs in theme_watch_stub.c take over and report the feature as
 * unavailable.
 */

#include "theme_watch_internal.h"

#ifdef IRIS_HAVE_PORTAL_WATCH

/* _GNU_SOURCE is provided by the build system (add_project_arguments). */
#include <iris/theme.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>

/* Single watcher per process — sufficient for one running app. */
static sd_bus *g_bus = NULL;
static iris_color_scheme_changed_fn g_cb = NULL;
static void *g_user = NULL;

/* SettingChanged signal signature:
 *   s  namespace   e.g. "org.freedesktop.appearance"
 *   s  key         e.g. "color-scheme"
 *   v  value       variant; for color-scheme it is uint32 */
static int on_setting_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;

    const char *ns = NULL;
    const char *key = NULL;
    if (sd_bus_message_read(m, "ss", &ns, &key) < 0)
        return 0;

    if (strcmp(ns, "org.freedesktop.appearance") != 0 || strcmp(key, "color-scheme") != 0) {
        /* Not the property we care about; skip. Still need to skip the
         * variant body so the message is fully parsed. */
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

    if (!g_cb)
        return 0;
    g_cb((iris_color_scheme)value, g_user);
    return 0;
}

IRIS_API int iris_watch_system_color_scheme(iris_color_scheme_changed_fn cb, void *user) {
    if (g_bus) {
        /* Already watching — update the callback and re-seed. */
        g_cb = cb;
        g_user = user;
        return 0;
    }
    if (!cb)
        return -1;

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

    if (rc < 0) {
        sd_bus_unref(g_bus);
        g_bus = NULL;
        return -1;
    }

    g_cb = cb;
    g_user = user;
    return 0;
}

IRIS_API int iris_color_scheme_watcher_fd(void) {
    if (!g_bus)
        return -1;
    int fd = sd_bus_get_fd(g_bus);
    return fd < 0 ? -1 : fd;
}

IRIS_API short iris_color_scheme_watcher_poll_events(void) {
    if (!g_bus)
        return 0;
    int ev = sd_bus_get_events(g_bus);
    return ev < 0 ? 0 : (short)ev;
}

IRIS_API void iris_pump_color_scheme_watcher(void) {
    if (!g_bus)
        return;
    /* Process pending messages; 0 = nothing to do, >0 = handled,
     * <0 = error (clean up on connection failure). */
    for (;;) {
        int rc = sd_bus_process(g_bus, NULL);
        if (rc < 0) {
            if (rc == -ENOTCONN || rc == -ECONNRESET || rc == -EPIPE || rc == -EBADF) {
                iris_stop_color_scheme_watcher();
            }
            break;
        }
        if (rc == 0)
            break;
    }
}

IRIS_API void iris_stop_color_scheme_watcher(void) {
    if (!g_bus)
        return;
    sd_bus_unref(g_bus);
    g_bus = NULL;
    g_cb = NULL;
    g_user = NULL;
}

#endif /* IRIS_HAVE_PORTAL_WATCH */
