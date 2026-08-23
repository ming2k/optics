/* test_platform_input.c — platform-neutral input mapping policy.
 *
 * Headless: the shared input-layer policy (platform_input.h) decides
 * things every backend must agree on. Two policies live there today:
 *
 *   - pointer button mapping (native code → lens mouse index)
 *   - scroll-channel classification: which lens_input field a scroll
 *     delta belongs on. A two-finger touchpad scroll and a notched wheel
 *     deliver byte-identical wl_pointer.axis events on Wayland — only
 *     the axis source distinguishes them — so the routing decision is
 *     pinned down here once, for every backend, instead of being
 *     re-derived (and re-broken) per platform.
 */

#include "platform_input.h"

#include "test_helpers.h"

#include <lens/lens.h>

int main(void) {
    /* --- pointer button mapping -------------------------------------- */

    CHECK(iris_pointer_button_to_lens(IRIS_POINTER_BUTTON_LEFT) == LENS_MOUSE_LEFT);
    CHECK(iris_pointer_button_to_lens(IRIS_POINTER_BUTTON_RIGHT) == LENS_MOUSE_RIGHT);
    CHECK(iris_pointer_button_to_lens(IRIS_POINTER_BUTTON_MIDDLE) == LENS_MOUSE_MIDDLE);
    /* Unknown / native-only buttons have no lens slot; backends drop. */
    CHECK(iris_pointer_button_to_lens(IRIS_POINTER_BUTTON_UNKNOWN) == -1);

    /* --- scroll-channel classification ------------------------------- */

    /* WL_POINTER_AXIS_SOURCE_WHEEL (0): a notched wheel — wheel steps. */
    CHECK(!iris_scroll_source_is_continuous(0));
    /* WL_POINTER_AXIS_SOURCE_FINGER (1): touchpad two-finger scroll —
     * continuous pixel deltas. */
    CHECK(iris_scroll_source_is_continuous(1));
    /* WL_POINTER_AXIS_SOURCE_CONTINUOUS (2): e.g. high-resolution wheel
     * emulation — continuous. */
    CHECK(iris_scroll_source_is_continuous(2));
    /* WL_POINTER_AXIS_SOURCE_WHEEL_TILT (3): tilt is a wheel-style
     * discrete gesture — steps, and its axis is horizontal anyway. */
    CHECK(!iris_scroll_source_is_continuous(3));

    /* The notch equivalence on the continuous channel must match the
     * lens-side consumer convention (slider.c divides scroll_pixels_y
     * by 40), or consumers folding both channels double- or half-count
     * a notch. */
    CHECK(IRIS_SCROLL_PIXELS_PER_NOTCH == 40.0);

    return TEST_REPORT();
}
