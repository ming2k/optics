/* test_theme.c — system colour-scheme query (theme_linux.c).
 *
 * Headless. The query consults, in order: gsettings → GTK_THEME env →
 * PREFER_DARK default. gsettings is host-dependent (GNOME boxes return a
 * real value; CI boxes have no schemas), so to exercise the GTK_THEME
 * branch deterministically we shadow `gsettings` on PATH with a shim that
 * exits non-zero. That forces the query through GTK_THEME / the default.
 */

/* _GNU_SOURCE is provided by the build system. */
#include <iris/theme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_helpers.h"

/* Create /tmp/iris-test-XXXXXX/gsettings that exits 1; return the dir path.
 * Caller must clean up (rmdir) the directory. Returns 0 on success. */
static int make_failing_gsettings(char *out_dir, size_t cap) {
    snprintf(out_dir, cap, "/tmp/iris-test-XXXXXX");
    if (!mkdtemp(out_dir))
        return -1;
    char path[1024];
    snprintf(path, sizeof path, "%s/gsettings", out_dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "#!/bin/sh\nexit 1\n");
    fclose(f);
    if (chmod(path, 0755) != 0)
        return -1;
    return 0;
}

static void with_env(const char *gtk_theme, iris_color_scheme *out) {
    if (gtk_theme)
        (void)setenv("GTK_THEME", gtk_theme, 1);
    else
        (void)unsetenv("GTK_THEME");
    *out = iris_query_system_color_scheme();
}

/* Watch callback for the lifecycle checks below: with no running
 * iris_app_run loop, deliveries are dropped before reaching it, so any
 * call here is a bug. */
static int watch_calls;
static void on_scheme_changed(iris_color_scheme scheme, void *user) {
    (void)scheme;
    (void)user;
    watch_calls++;
}

int main(void) {
    /* 1. Build the failing-gsettings sandbox and prepend it to PATH so the
     *    popen("gsettings ...") in theme_linux.c hits our shim. */
    char dir[256];
    CHECK(make_failing_gsettings(dir, sizeof dir) == 0);

    const char *old_path = getenv("PATH");
    char new_path[4096];
    snprintf(new_path, sizeof new_path, "%s:%s", dir, old_path ? old_path : "/usr/bin");
    (void)setenv("PATH", new_path, 1);

    /* 2. GTK_THEME dark variants → PREFER_DARK (deterministic). */
    iris_color_scheme s;

    with_env("dark:Adwaita", &s);
    CHECK(s == IRIS_COLOR_SCHEME_PREFER_DARK);

    with_env("Adwaita:dark", &s);
    CHECK(s == IRIS_COLOR_SCHEME_PREFER_DARK);

    with_env("Adwaita-dark", &s);
    CHECK(s == IRIS_COLOR_SCHEME_PREFER_DARK);

    /* 3. GTK_THEME with no dark marker falls through to the safe default. */
    with_env("Adwaita", &s);
    CHECK(s == IRIS_COLOR_SCHEME_PREFER_DARK);

    /* 4. GTK_THEME unset also falls through to the safe default. */
    with_env(NULL, &s);
    CHECK(s == IRIS_COLOR_SCHEME_PREFER_DARK);

    /* 5. iris_system_prefers_dark is consistent: PREFER_DARK and
     *    NO_PREFERENCE both count as "dark" (the library's safe default). */
    CHECK(iris_system_prefers_dark() == true);

    /* 6. Enum values are exactly the documented discriminants. */
    CHECK(IRIS_COLOR_SCHEME_NO_PREFERENCE == 0);
    CHECK(IRIS_COLOR_SCHEME_PREFER_DARK == 1);
    CHECK(IRIS_COLOR_SCHEME_PREFER_LIGHT == 2);

    /* 7. Live watching is callback-based (no fd/poll surface). Headless we
     *    cannot force a desktop change, so exercise the lifecycle contract:
     *      - unwatch without watch is a safe no-op;
     *      - watch(NULL) is rejected;
     *      - watch either succeeds (libsystemd + a reachable session bus)
     *        or cleanly reports -1; on success a second watch replaces the
     *        registration and unwatch stops it;
     *      - no callback fires spuriously while we sit idle. */
    iris_color_scheme_unwatch(); /* safe no-op before any watch */
    CHECK(iris_color_scheme_watch(NULL, NULL) != 0);

    watch_calls = 0;
    if (iris_color_scheme_watch(on_scheme_changed, NULL) == 0) {
        /* Re-registration replaces cb/user and keeps watching. */
        CHECK(iris_color_scheme_watch(on_scheme_changed, NULL) == 0);
        iris_color_scheme_unwatch();
        /* After unwatch returns the registration is dead: the callback
         * must not fire, and a second unwatch is a safe no-op. */
        iris_color_scheme_unwatch();
        CHECK(watch_calls == 0);
    }
    /* -1 path (no libsystemd at build time / no session bus in CI): the
     * startup-only query above already covered the fallback. */

    /* 8. Clean up the sandbox (best-effort). */
    char shim[1024];
    snprintf(shim, sizeof shim, "%s/gsettings", dir);
    (void)unlink(shim);
    (void)rmdir(dir);

    return TEST_REPORT();
}
