/*
 * a11y_prefs_linux.c — system accessibility-preference query (Linux).
 *
 * Modern Freedesktop / Wayland architecture (ADR-0075, ADR-0103):
 * Reads user accessibility preferences via the standard XDG Desktop Portal
 * (org.freedesktop.portal.Settings) over D-Bus with a strict 50ms fail-fast
 * timeout. Zero subprocess spawning, zero shell forks (popen), zero disk I/O.
 *
 * Strategy (in priority order):
 *   1. XDG Desktop Portal Settings over session D-Bus:
 *        org.gnome.desktop.interface enable-animations (bool)
 *        org.freedesktop.appearance contrast (uint32 0/1, standard)
 *        org.gnome.desktop.interface text-scaling-factor (double)
 *   2. Safe library defaults (false / false / 1.0f).
 *
 * Live watching is provided by theme_watch_portal.c on the shared portal pump.
 */

#include <iris/a11y_prefs.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#ifdef IRIS_HAVE_PORTAL_WATCH
#include <systemd/sd-bus.h>

static bool portal_read_setting(sd_bus *bus, const char *ns, const char *key,
                                char type_char, void *out) {
    if (!bus)
        return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop",
                                "/org/freedesktop/portal/desktop",
                                "org.freedesktop.portal.Settings",
                                "ReadOne", &err, &reply, "ss", ns, key);
    if (rc < 0) {
        sd_bus_error_free(&err);
        rc = sd_bus_call_method(bus, "org.freedesktop.portal.Desktop",
                                "/org/freedesktop/portal/desktop",
                                "org.freedesktop.portal.Settings",
                                "Read", &err, &reply, "ss", ns, key);
    }
    if (rc < 0) {
        sd_bus_error_free(&err);
        return false;
    }

    if (sd_bus_message_enter_container(reply, 'v', NULL) < 0) {
        sd_bus_message_unref(reply);
        return false;
    }

    bool ok = false;
    if (type_char == 'b') {
        int val = 0;
        if (sd_bus_message_read(reply, "b", &val) >= 0) {
            *(bool *)out = (val != 0);
            ok = true;
        }
    } else if (type_char == 'u') {
        uint32_t val = 0;
        if (sd_bus_message_read(reply, "u", &val) >= 0) {
            *(uint32_t *)out = val;
            ok = true;
        }
    } else if (type_char == 'd') {
        double val = 0.0;
        if (sd_bus_message_read(reply, "d", &val) >= 0) {
            *(double *)out = val;
            ok = true;
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    return ok;
}
#endif /* IRIS_HAVE_PORTAL_WATCH */
#endif /* __linux__ */

IRIS_API iris_a11y_prefs iris_a11y_prefs_query(void) {
    iris_a11y_prefs p = {.reduced_motion = false, .high_contrast = false, .text_scale = 1.0f};

#ifdef __linux__
#ifdef IRIS_HAVE_PORTAL_WATCH
    sd_bus *session = NULL;
    if (sd_bus_open_user(&session) >= 0) {
        (void)sd_bus_set_method_call_timeout(session, 50000); /* 50ms fail-fast */

        bool anim = true;
        if (portal_read_setting(session, "org.gnome.desktop.interface", "enable-animations", 'b', &anim))
            p.reduced_motion = !anim;

        uint32_t contrast = 0;
        if (portal_read_setting(session, "org.freedesktop.appearance", "contrast", 'u', &contrast)) {
            p.high_contrast = (contrast == 1);
        } else {
            bool hc = false;
            if (portal_read_setting(session, "org.gnome.desktop.a11y.interface", "high-contrast", 'b', &hc))
                p.high_contrast = hc;
        }

        double scale = 1.0;
        if (portal_read_setting(session, "org.gnome.desktop.interface", "text-scaling-factor", 'd', &scale)) {
            if (scale > 0.0 && scale <= 10.0)
                p.text_scale = (float)scale;
        }

        sd_bus_unref(session);
    }
#endif /* IRIS_HAVE_PORTAL_WATCH */
#endif /* __linux__ */

    return p;
}
