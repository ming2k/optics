/* a11y_prefs_linux.c — system accessibility-preference query (Linux).
 *
 * Strategy, in priority order (mirrors theme_linux.c):
 *   1. gsettings (GNOME 42+, Cinnamon, MATE, Unity): the portal proxies
 *      these same keys, so the values watched live by theme_watch_portal.c
 *      are read here at startup:
 *        org.gnome.desktop.interface text-scaling-factor  (double, 1.0)
 *        org.gnome.desktop.interface enable-animations    (bool)
 *        org.gnome.desktop.a11y.interface high-contrast   (bool, GNOME <46)
 *      plus org.freedesktop.appearance contrast (uint32 0/1) where the
 *      desktop publishes it (GNOME 46+).
 *   2. Environment hints for non-gsettings desktops: KDE writes
 *      kdeglobals; a popen-per-key probe is already the established
 *      pattern here, so the same probe reads kreadconfig5/6 where
 *      present.
 *   3. Library defaults (false / false / 1.0) — the correct accessible
 *      baseline, never an error.
 *
 * Startup queries only; live watching is theme_watch_portal.c (one shared
 * portal-settings pump).
 */

/* _GNU_SOURCE is provided by the build system (add_project_arguments). */
#include <iris/a11y_prefs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__

/* Run `cmd`, read up to cap-1 bytes of stdout into buf. Returns 0 when the
 * command produced output. Same fork-and-read pattern as
 * query_gsettings_color_scheme (theme_linux.c): faster than a full D-Bus
 * round trip and dependency-free. */
static int probe(const char *cmd, char *buf, size_t cap) {
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    size_t n = fread(buf, 1, cap - 1, fp);
    int status = pclose(fp);
    if (n == 0 || status != 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static bool query_gsettings_bool(const char *schema, const char *key) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    char buf[64];
    if (probe(cmd, buf, sizeof buf) != 0)
        return false;
    /* gsettings prints booleans bare ("true"/"false"). */
    return strstr(buf, "true") != NULL;
}

static float query_gsettings_double(const char *schema, const char *key) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "gsettings get %s %s 2>/dev/null", schema, key);
    char buf[64];
    if (probe(cmd, buf, sizeof buf) != 0)
        return 1.0f;
    float v = strtof(buf, NULL);
    /* Reject nonsense: a text scale must be a sane positive multiplier. */
    if (v <= 0.0f || v > 10.0f)
        return 1.0f;
    return v;
}

/* org.freedesktop.appearance contrast — the cross-desktop key the portal
 * standardised (GNOME 46+; KDE follows). gsettings exposes it through the
 * portal shim on GNOME; on KDE it lives in kdeglobals. */
static bool query_contrast(void) {
    char buf[64];
    if (probe("gsettings get org.gnome.desktop.interface contrast 2>/dev/null", buf, sizeof buf) ==
        0)
        return strstr(buf, "1") != NULL; /* "1" = high contrast requested */
    if (probe("kreadconfig6 --group KDE --key contrast 2>/dev/null", buf, sizeof buf) == 0 ||
        probe("kreadconfig5 --group KDE --key contrast 2>/dev/null", buf, sizeof buf) == 0)
        return strstr(buf, "1") != NULL;
    return query_gsettings_bool("org.gnome.desktop.a11y.interface", "high-contrast");
}

static float query_text_scale(void) {
    float v = query_gsettings_double("org.gnome.desktop.interface", "text-scaling-factor");
    if (v != 1.0f)
        return v;
    /* KDE: kdeglobals [General] fontPackageScale — read via kreadconfig. */
    char buf[64];
    if (probe("kreadconfig6 --group General --key fontPackageScale 2>/dev/null", buf, sizeof buf) ==
            0 ||
        probe("kreadconfig5 --group General --key fontPackageScale 2>/dev/null", buf, sizeof buf) ==
            0) {
        float k = strtof(buf, NULL);
        if (k > 0.0f && k <= 10.0f)
            return k;
    }
    return 1.0f;
}

#endif /* __linux__ */

IRIS_API iris_a11y_prefs iris_a11y_prefs_query(void) {
    iris_a11y_prefs p = {.reduced_motion = false, .high_contrast = false, .text_scale = 1.0f};
#ifdef __linux__
    p.reduced_motion = !query_gsettings_bool("org.gnome.desktop.interface", "enable-animations");
    p.high_contrast = query_contrast();
    p.text_scale = query_text_scale();
#endif
    return p;
}
