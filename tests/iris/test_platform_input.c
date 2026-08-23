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

    /* --- scroll sign convention (iris_scroll_accum) --------------------
     *
     * Facts pinned here (cross-checked against lens consumers):
     *   - Platform events arrive positive = physical wheel-down /
     *     finger-down (wl_pointer.axis contract).
     *   - Lens's contract on BOTH channels: negative = the user's down
     *     gesture (tests/lens/test_scroll.c: scroll_y = -5 reveals later
     *     content), positive = up.
     * iris_scroll_accum performs that single inversion at the platform
     * boundary; nothing downstream may invert again. */

    {
        double sy = 0, py = 0, sx = 0, px = 0;
        /* Wheel-down on Wayland: wl_pointer.axis vertical +10 units.
         * The caller (ptr_axis) normalises ~10 units to one notch first;
         * this helper takes the already-normalised delta. One notch of
         * down gesture -> scroll_y == -1 (down is negative in lens). */
        iris_scroll_accum(&sy, &py, &sx, &px, 0, 1.0, false);
        CHECK(sy == -1.0 && py == 0.0 && sx == 0.0 && px == 0.0);

        /* Wheel-up cancels it: -1 notch -> scroll_y back to 0. */
        iris_scroll_accum(&sy, &py, &sx, &px, 0, -1.0, false);
        CHECK(sy == 0.0 && py == 0.0);

        /* Two-finger touchpad down 25 logical px: continuous vertical.
         * Same sign inversion, pixel channel, untouched step channel. */
        double sy2 = 0, py2 = 0, sx2 = 0, px2 = 0;
        iris_scroll_accum(&sy2, &py2, &sx2, &px2, 0, 25.0, true);
        CHECK(sy2 == 0.0 && py2 == -25.0);

        /* Horizontal wheel tilt right (+1 notch) and touchpad right
         * (+25 px): horizontal channels, same inversion. */
        double sy3 = 0, py3 = 0, sx3 = 0, px3 = 0;
        iris_scroll_accum(&sy3, &py3, &sx3, &px3, 1, 1.0, false);
        CHECK(sx3 == -1.0 && px3 == 0.0 && sy3 == 0.0 && py3 == 0.0);
        iris_scroll_accum(&sy3, &py3, &sx3, &px3, 1, 25.0, true);
        CHECK(sx3 == -1.0 && px3 == -25.0);

        /* Accumulation is additive across events in a frame group. */
        double sy4 = 0, py4 = 0, sx4 = 0, px4 = 0;
        for (int i = 0; i < 5; i++)
            iris_scroll_accum(&sy4, &py4, &sx4, &px4, 0, 1.0, false);
        CHECK(sy4 == -5.0 && py4 == 0.0);
    }

    return TEST_REPORT();
}
