/* platform_input.h — platform-neutral pointer button mapping.
 *
 * lens's input contract is indexed by LENS_MOUSE_LEFT / LENS_MOUSE_RIGHT /
 * LENS_MOUSE_MIDDLE (see lens/lens.h). Each backend receives native button
 * codes — Linux evdev BTN_* (linux/input-event-codes.h), Win32
 * WM_xBUTTONDOWN / WM_xBUTTONUP messages, Cocoa NSEvent buttonNumber — and
 * must map them onto that contract. This header is the shared mapping
 * layer so no backend ever includes another platform's headers:
 *
 *   1. Translate the native code to iris_pointer_button (per-backend switch;
 *      the only place linux/input-event-codes.h, windows.h button codes,
 *      or NSEvent constants may appear).
 *   2. Convert with iris_pointer_button_to_lens() to the lens index.
 *
 * Unknown/native-only buttons (X1/X2, buttonNumber ≥ 3, …) translate to
 * IRIS_POINTER_BUTTON_UNKNOWN and map to -1; backends must ignore those
 * events, as app_wayland.c does today.
 *
 * It is also the shared home for the scroll-channel classification
 * (below): pure, dependency-free, unit-testable headlessly
 * (tests/iris/test_platform_input.c).
 */
#ifndef IRIS_PLATFORM_INPUT_H
#define IRIS_PLATFORM_INPUT_H

#include <lens/lens.h>

typedef enum iris_pointer_button {
    IRIS_POINTER_BUTTON_UNKNOWN = -1,
    IRIS_POINTER_BUTTON_LEFT = 0,
    IRIS_POINTER_BUTTON_RIGHT,
    IRIS_POINTER_BUTTON_MIDDLE,
} iris_pointer_button;

/* lens_input mouse index for `button`, or -1 when the button has no lens
 * slot (caller must drop the event). */
static inline int iris_pointer_button_to_lens(iris_pointer_button button) {
    switch (button) {
    case IRIS_POINTER_BUTTON_LEFT:
        return LENS_MOUSE_LEFT;
    case IRIS_POINTER_BUTTON_RIGHT:
        return LENS_MOUSE_RIGHT;
    case IRIS_POINTER_BUTTON_MIDDLE:
        return LENS_MOUSE_MIDDLE;
    case IRIS_POINTER_BUTTON_UNKNOWN:
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Scroll-channel classification                                      */
/* ------------------------------------------------------------------ */

/* A wheel notch, expressed on the continuous (scroll_pixels_*) channel,
 * matching the lens-side consumer convention (slider.c divides
 * scroll_pixels_y by 40; win32 routes sub-notch deltas at 40 px/notch).
 * Consumers that fold both channels into one distance can multiply wheel
 * steps by this constant. */
#define IRIS_SCROLL_PIXELS_PER_NOTCH 40.0

/* Which lens_input channel a scroll delta belongs on (ADR-0036): wheel
 * steps (scroll_x/y — discrete notches, widgets scale them by their line
 * factor) or continuous pixels (scroll_pixels_x/y — touchpad finger
 * motion, widgets consume unscaled). */
typedef enum iris_scroll_channel {
    IRIS_SCROLL_WHEEL_STEPS = 0,
    IRIS_SCROLL_PIXELS,
} iris_scroll_channel;

/* True when `source` is a continuous/finger-driven axis source whose
 * deltas are pixel distances, not wheel notches. This is a policy
 * question every backend must answer the same way (Wayland axis_source,
 * Win32 high-resolution wheels, Cocoa hasPreciseScrollingDeltas), so it
 * lives here rather than being re-decided per backend. */
static inline bool iris_scroll_source_is_continuous(uint32_t source) {
    /* Mirrors the WL_POINTER_AXIS_SOURCE_* enum: 0 wheel, 1 finger, 2
     * continuous, 3 wheel tilt. Numeric on purpose — this header must
     * stay compilable where wayland-client headers are unavailable
     * (win32/cocoa backends, headless tests). */
    return source == 1     /* WL_POINTER_AXIS_SOURCE_FINGER      */
           || source == 2; /* WL_POINTER_AXIS_SOURCE_CONTINUOUS */
}

/* ------------------------------------------------------------------ */
/*  Scroll sign convention                                             */
/* ------------------------------------------------------------------ */

/* Direction contract for BOTH channels (ADR-0036 established the two
 * channels; this documents the sign they share): positive = the user's
 * gesture pushes content up (wheel rolled away from the palm / finger
 * moved up); negative = content moves up under a down gesture. Lens
 * consumers pin the same convention (tests/lens/test_scroll.c,
 * test_slider.c): negative scroll_y scrolls the viewport DOWN the
 * document.
 *
 * Platform events arrive with the OPPOSITE sign on every backend this
 * wayland-first stack targets (wl_pointer.axis positive = physical
 * wheel-down; WM_MOUSEWHEEL positive = wheel rotated away = content
 * scrolls up — see each backend's caller for its exact note). The
 * inversion happens exactly once, at the platform boundary, through this
 * helper — never again downstream.
 *
 * `axis` is 0 for vertical, 1 for horizontal (the WL_POINTER_AXIS_*
 * values; passed numerically so the header stays wayland-free). */
static inline void iris_scroll_accum(double *steps_y, double *pixels_y, double *steps_x,
                                     double *pixels_x, uint32_t axis, double delta,
                                     bool continuous) {
    if (axis == 0) { /* WL_POINTER_AXIS_VERTICAL_SCROLL */
        if (continuous)
            *pixels_y -= delta;
        else
            *steps_y -= delta;
    } else { /* WL_POINTER_AXIS_HORIZONTAL_SCROLL */
        if (continuous)
            *pixels_x -= delta;
        else
            *steps_x -= delta;
    }
}

#endif /* IRIS_PLATFORM_INPUT_H */
