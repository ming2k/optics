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

#endif /* IRIS_PLATFORM_INPUT_H */
