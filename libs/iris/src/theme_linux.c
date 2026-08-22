/* theme_linux.c — system colour-scheme query (Linux).
 *
 * Strategy (in priority order):
 *   1. gsettings: org.gnome.desktop.interface color-scheme
 *      GNOME 42+, Cinnamon, MATE, Unity — covers most desktops that ship gsettings.
 *   2. org.freedesktop.portal.Settings via gdbus (works on any portal-aware DE:
 *      KDE Plasma 5.27+, GNOME via portal backend, etc.). Skipped for now —
 *      gsettings covers the dominant case and subprocess fork is faster than a
 *      full D-Bus round-trip.
 *   3. GTK_THEME env var (legacy hint, prefix "dark:" or ":dark" suffix).
 *   4. Fall back to IRIS_COLOR_SCHEME_PREFER_DARK — a UI library defaulting
 *      to dark is safer than defaulting to light (dark is the lower-glare
 *      choice when the user's preference is unknown).
 *
 * Startup queries only; live watching is provided by theme_watch_portal.c
 * (see theme_watch_internal.h).
 */

/* _GNU_SOURCE is provided by the build system (add_project_arguments). */
#include <iris/theme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
static int query_gsettings_color_scheme(char *buf, size_t cap) {
    /* gsettings returns the value in single quotes, e.g. 'prefer-dark'.
     * Empty value (which gsettings returns for "default") is ''. */
    FILE *fp = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (!fp)
        return -1;
    size_t n = fread(buf, 1, cap - 1, fp);
    pclose(fp);
    if (n == 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int query_gtk_theme_env(char *buf, size_t cap) {
    const char *t = getenv("GTK_THEME");
    if (!t || !*t)
        return -1;
    strncpy(buf, t, cap - 1);
    buf[cap - 1] = '\0';
    return 0;
}
#endif /* __linux__ */

IRIS_API iris_color_scheme iris_query_system_color_scheme(void) {
#ifdef __linux__
    char buf[128];

    if (query_gsettings_color_scheme(buf, sizeof buf) == 0) {
        if (strstr(buf, "prefer-dark"))
            return IRIS_COLOR_SCHEME_PREFER_DARK;
        if (strstr(buf, "prefer-light"))
            return IRIS_COLOR_SCHEME_PREFER_LIGHT;
        /* "default" / empty → no preference. */
        return IRIS_COLOR_SCHEME_NO_PREFERENCE;
    }

    if (query_gtk_theme_env(buf, sizeof buf) == 0) {
        /* "dark" appears as a prefix (dark:Foo) or a suffix (Foo:dark). */
        for (char *p = buf; *p; ++p) {
            if (strncasecmp(p, "dark", 4) == 0)
                return IRIS_COLOR_SCHEME_PREFER_DARK;
        }
    }
#else
    /* On non-Linux platforms today's iris has no backend at all; fall
     * through to the dark default. */
#endif

    /* Safe default: dark (lower glare for unknown environments). */
    return IRIS_COLOR_SCHEME_PREFER_DARK;
}

IRIS_API bool iris_system_prefers_dark(void) {
    iris_color_scheme s = iris_query_system_color_scheme();
    return s == IRIS_COLOR_SCHEME_PREFER_DARK || s == IRIS_COLOR_SCHEME_NO_PREFERENCE;
}
