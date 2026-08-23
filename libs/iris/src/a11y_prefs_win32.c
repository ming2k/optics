/* a11y_prefs_win32.c — system accessibility-preference query + watch (Win32).
 *
 * Query (SystemParametersInfo, no registry scraping — SPI is the
 * documented, versioned surface for exactly these three settings):
 *   reduced_motion  SPI_GETCLIENTAREAANIMATION == FALSE. This is the
 *                   "Show animations in Windows" toggle (Settings >
 *                   Accessibility > Visual effects); FALSE means the user
 *                   asked Windows to stop animating client areas.
 *   high_contrast   SPI_GETHIGHCONTRAST + HCF_HIGHCONTRASTON.
 *   text_scale      PerMonitorV2 DPI already tracks the display scale;
 *                   the accessibility *text* scale rides
 *                   SystemParametersInfo(SPI_GETNONCLIENTMETRICS) message
 *                   metrics in the negative direction: Windows applies
 *                   "Make text bigger" by scaling message fonts, so the
 *                   factor is lfMessageFont.lfHeight against the default
 *                   -12 (9pt at 96 DPI). Clamped to [0.5, 5.0]; the exact
 *                   mapping matches what Win32 apps observe.
 *
 * Live watch needs no thread (same as theme_win32.c): WM_SETTINGCHANGE is
 * broadcast to top-level windows on the iris main thread's message loop.
 * iris_a11y_prefs_watch only registers the callback; app_win32.c calls
 * iris_a11y_prefs_win32__notify_setting_change() from WM_SETTINGCHANGE,
 * which re-queries and fires synchronously when the set changed.
 *
 * NOT YET VERIFIED ON A REAL WINDOWS MACHINE (zig compile-check only), the
 * same caveat theme_win32.c carries.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef IRIS_BUILDING
#define IRIS_BUILDING 1
#endif

#include <windows.h>

#include "a11y_prefs_internal.h"

#include <iris/a11y_prefs.h>

/* Registered watchers, touched on the iris main thread only. Two
 * independent slots: the host's public one and the platform backend's
 * (a11y_prefs_internal.h). */
static iris_a11y_prefs_changed_fn g_cb = NULL;
static void *g_user = NULL;
static iris_a11y_prefs_changed_fn g_backend_cb = NULL;
static void *g_backend_user = NULL;
static iris_a11y_prefs g_last;

IRIS_API iris_a11y_prefs iris_a11y_prefs_query(void) {
    iris_a11y_prefs p = {.reduced_motion = false, .high_contrast = false, .text_scale = 1.0f};

    BOOL animate = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0))
        p.reduced_motion = !animate;

    HIGHCONTRASTW hc = {.cbSize = sizeof hc};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0))
        p.high_contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;

    NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        /* lfHeight is negative (character height) for the default -12 at
         * 96 DPI; a positive value would be cell height, normalised by
         * dropping the internal leading heuristic — scale off the
         * magnitude either way. */
        float h = (float)ncm.lfMessageFont.lfHeight;
        if (h < 0.0f)
            h = -h;
        if (h > 0.0f) {
            float factor = h / 12.0f;
            if (factor >= 0.5f && factor <= 5.0f)
                p.text_scale = factor;
        }
    }
    return p;
}

IRIS_API int iris_a11y_prefs_watch(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    g_cb = cb;
    g_user = user;
    g_last = iris_a11y_prefs_query();
    return 0;
}

IRIS_API void iris_a11y_prefs_unwatch(void) {
    g_cb = NULL;
    g_user = NULL;
}

int iris_a11y_prefs__watch_backend(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    g_backend_cb = cb;
    g_backend_user = user;
    g_last = iris_a11y_prefs_query();
    return 0;
}

void iris_a11y_prefs__unwatch_backend(void) {
    g_backend_cb = NULL;
    g_backend_user = NULL;
}

/* Internal seam (a11y_prefs_internal.h contract): called by app_win32.c's
 * WndProc on WM_SETTINGCHANGE. Runs on the iris main thread; fires the
 * registered callbacks synchronously when the preference set changed. */
void iris_a11y_prefs_win32__notify_setting_change(void) {
    iris_a11y_prefs now = iris_a11y_prefs_query();
    if (now.reduced_motion != g_last.reduced_motion || now.high_contrast != g_last.high_contrast ||
        now.text_scale != g_last.text_scale) {
        g_last = now;
        if (g_cb)
            g_cb(&now, g_user);
        if (g_backend_cb)
            g_backend_cb(&now, g_backend_user);
    }
}
