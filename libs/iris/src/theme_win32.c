/* theme_win32.c — system colour-scheme query + live watch (Win32).
 *
 * Query: HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
 * "AppsUseLightTheme" (DWORD; 0 = dark). This is the setting the Windows 10/11
 * "Choose your default app mode" UI flips, and the same value UWP/WinUI apps
 * observe. Missing key or read failure falls back to PREFER_DARK — a UI
 * library defaulting to dark is the safer unknown-environment choice (same
 * policy as theme_linux.c).
 *
 * Live watch needs no watcher thread on Win32: the change signal is
 * WM_SETTINGCHANGE (lParam "ImmersiveColorSet"), which Windows broadcasts to
 * top-level windows — i.e. it already arrives on the iris main thread's
 * message loop. iris_color_scheme_watch therefore only registers the
 * callback; app_win32.c's WndProc calls iris_theme_win32__notify_setting_change()
 * from WM_SETTINGCHANGE, and the callback fires synchronously on the loop
 * thread, satisfying theme.h's main-thread delivery guarantee without
 * detouring through the wakeup seam.
 *
 * NOT YET VERIFIED ON A REAL WINDOWS MACHINE (zig compile-check only). In
 * particular: whether every dark-mode toggle path reliably broadcasts
 * WM_SETTINGCHANGE to per-monitor-V2 windows, and whether the registry value
 * is already updated when the message arrives (both are the documented
 * behaviour, but deserve one on-hardware pass).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/* meson supplies -DIRIS_BUILDING=1; define it so single-file compile checks
 * (tools/zig-win32-check.sh) don't see dllimport on definitions. */
#ifndef IRIS_BUILDING
#define IRIS_BUILDING 1
#endif

#include <windows.h>

#include <iris/theme.h>

#include "theme_watch_internal.h"

/* Registered watchers, touched on the iris main thread only (watch/unwatch
 * are documented main-thread-only, and the notify hook runs inside the
 * WndProc on the same thread). Two independent slots: the host's public one
 * and the platform backend's internal one (theme_watch_internal.h) — the
 * backend must not consume or overwrite the host's registration. */
static iris_color_scheme_changed_fn g_cb = NULL;
static void *g_user = NULL;
static iris_color_scheme_changed_fn g_backend_cb = NULL;
static void *g_backend_user = NULL;
static iris_color_scheme g_last = IRIS_COLOR_SCHEME_PREFER_DARK;

IRIS_API iris_color_scheme iris_query_system_color_scheme(void) {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD value = 1, cb = sizeof value;
        LSTATUS st = RegQueryValueExW(hkey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &cb);
        RegCloseKey(hkey);
        if (st == ERROR_SUCCESS)
            return value == 0 ? IRIS_COLOR_SCHEME_PREFER_DARK : IRIS_COLOR_SCHEME_PREFER_LIGHT;
    }
    /* Safe default: dark (lower glare for unknown environments). */
    return IRIS_COLOR_SCHEME_PREFER_DARK;
}

IRIS_API bool iris_system_prefers_dark(void) {
    iris_color_scheme s = iris_query_system_color_scheme();
    return s == IRIS_COLOR_SCHEME_PREFER_DARK || s == IRIS_COLOR_SCHEME_NO_PREFERENCE;
}

IRIS_API int iris_color_scheme_watch(iris_color_scheme_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    g_cb = cb;
    g_user = user;
    /* Seed the compare value: the notify hook only fires on an actual flip,
     * and the current value is NOT reported at registration (theme.h). */
    g_last = iris_query_system_color_scheme();
    return 0;
}

IRIS_API void iris_color_scheme_unwatch(void) {
    /* No thread to join, no handle to close — just drop the registration.
     * A change detected after this is silently discarded. */
    g_cb = NULL;
    g_user = NULL;
}

int iris_theme__watch_backend(iris_color_scheme_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    g_backend_cb = cb;
    g_backend_user = user;
    g_last = iris_query_system_color_scheme();
    return 0;
}

void iris_theme__unwatch_backend(void) {
    g_backend_cb = NULL;
    g_backend_user = NULL;
}

/* Internal seam (not in any public header): called by app_win32.c's WndProc
 * on WM_SETTINGCHANGE. Runs on the iris main thread; fires the registered
 * callbacks synchronously when the scheme actually changed. */
void iris_theme_win32__notify_setting_change(void) {
    iris_color_scheme now = iris_query_system_color_scheme();
    if (now != g_last) {
        g_last = now;
        if (g_cb)
            g_cb(now, g_user);
        if (g_backend_cb)
            g_backend_cb(now, g_backend_user);
    }
}
