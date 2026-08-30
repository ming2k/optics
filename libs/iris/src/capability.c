/* capability.c — static feature discovery (see iris/capability.h).
 *
 * One translation unit, no backend headers: the backend answers are
 * compile-time constants chosen by the IRIS_BACKEND_* defines that meson
 * sets per target (ADR-0044). Keeping every capability answer in this
 * single table is the point — a capability that is not wired here is
 * reported as unsupported everywhere, consistently, instead of being
 * discovered per-call by silence.
 */

#include <iris/capability.h>

/* ------------------------------------------------------------------ */
/*  Backend identity                                                  */
/* ------------------------------------------------------------------ */

#if defined(IRIS_BACKEND_WAYLAND)
#define IRISI_BACKEND_NAME "wayland"
#elif defined(IRIS_BACKEND_WIN32)
#define IRISI_BACKEND_NAME "win32"
#elif defined(IRIS_BACKEND_COCOA)
#define IRISI_BACKEND_NAME "cocoa"
#else
#define IRISI_BACKEND_NAME "none"
#endif

const char *iris_backend_name(void) {
    return IRISI_BACKEND_NAME;
}

/* ------------------------------------------------------------------ */
/*  Capability table                                                  */
/* ------------------------------------------------------------------ */
/* Tablet and drop-target are Wayland-only because the protocol surface
 * (zwp_tablet_v2, wl_data_device offers) is only wired there; pen input
 * degrades to synthesized mouse events elsewhere. Primary selection is
 * Wayland-only by platform design. Fractional scale: Win32 carries true
 * per-monitor DPI into the scale factor today; Wayland quantizes to the
 * nearest integer buffer scale and Cocoa reports integer
 * backingScaleFactor (ADR-0067). */

static const int irisi_caps_wayland[] = {
    1, /* IRIS_CAP_WINDOW_CONTROL  */
    1, /* IRIS_CAP_THEME_WATCH     */
    1, /* IRIS_CAP_A11Y            */
    1, /* IRIS_CAP_FILE_DIALOG     */
    1, /* IRIS_CAP_CLIPBOARD       */
    1, /* IRIS_CAP_PRIMARY_SELECTION */
    1, /* IRIS_CAP_TABLET          */
    1, /* IRIS_CAP_DROP_TARGET     */
    0, /* IRIS_CAP_DECORATIONS — runtime: requires the compositor's
        * xdg-decoration protocol; the static answer is "not guaranteed".
        * A runtime-refined variant can flip this when the globals land. */
    0, /* IRIS_CAP_FRACTIONAL_SCALE */
    1, /* IRIS_CAP_DRAG_SOURCE     */
};

static const int irisi_caps_win32[] = {
    1, /* WINDOW_CONTROL  */
    1, /* THEME_WATCH     */
    0, /* A11Y — stub (ADR-0056 D5) */
    1, /* FILE_DIALOG     */
    1, /* CLIPBOARD       */
    0, /* PRIMARY_SELECTION — platform has none */
    0, /* TABLET          */
    0, /* DROP_TARGET     */
    1, /* DECORATIONS — native */
    1, /* FRACTIONAL_SCALE — per-monitor DPI v2 */
    0, /* DRAG_SOURCE     */
};

static const int irisi_caps_cocoa[] = {
    1, /* WINDOW_CONTROL  */
    1, /* THEME_WATCH     */
    0, /* A11Y — stub (ADR-0056 D5) */
    1, /* FILE_DIALOG     */
    1, /* CLIPBOARD       */
    0, /* PRIMARY_SELECTION */
    0, /* TABLET          */
    0, /* DROP_TARGET     */
    1, /* DECORATIONS — native */
    0, /* FRACTIONAL_SCALE — integer backingScaleFactor */
    0, /* DRAG_SOURCE     */
};

static const int irisi_caps_none[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define IRISI_CAP_COUNT ((int)(sizeof(irisi_caps_wayland) / sizeof(irisi_caps_wayland[0])))

/* Compile-time guard: every table must answer every capability. A new
 * enum value that is not wired into all four tables fails the build
 * here rather than reading out of bounds at runtime. */
_Static_assert(sizeof(irisi_caps_win32) == sizeof(irisi_caps_wayland), "capability table drift");
_Static_assert(sizeof(irisi_caps_cocoa) == sizeof(irisi_caps_wayland), "capability table drift");
_Static_assert(sizeof(irisi_caps_none) == sizeof(irisi_caps_wayland), "capability table drift");

int iris_supports(iris_capability cap) {
    if ((int)cap < 0 || (int)cap >= IRISI_CAP_COUNT)
        return 0; /* forward-compat: unknown newer values */
#if defined(IRIS_BACKEND_WAYLAND)
    return irisi_caps_wayland[(int)cap];
#elif defined(IRIS_BACKEND_WIN32)
    return irisi_caps_win32[(int)cap];
#elif defined(IRIS_BACKEND_COCOA)
    return irisi_caps_cocoa[(int)cap];
#else
    return irisi_caps_none[(int)cap];
#endif
}
