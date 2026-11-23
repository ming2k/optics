/* test_platform_input.c — platform-neutral input mapping policy.
 *
 * Headless: the shared input-layer policy (platform_input.h) decides
 * things every backend must agree on. Two policies live there today:
 *
 *   - pointer button mapping (native code → ui mouse index)
 *   - scroll-channel classification: which optics_window_input field a scroll
 *     delta belongs on. A two-finger touchpad scroll and a notched wheel
 *     deliver byte-identical wl_pointer.axis events on Wayland — only
 *     the axis source distinguishes them — so the routing decision is
 *     pinned down here once, for every backend, instead of being
 *     re-derived (and re-broken) per platform.
 */

#include "platform_input.h"

#include "optics_test.h"

#include <optics/window/host.h>

int main(void) {
    /* --- pointer button mapping -------------------------------------- */

    OT_EXPECT(optics_window_pointer_button_index(OPTICS_WINDOW_POINTER_BUTTON_LEFT) ==
              OPTICS_WINDOW_BUTTON_LEFT);
    OT_EXPECT(optics_window_pointer_button_index(OPTICS_WINDOW_POINTER_BUTTON_RIGHT) ==
              OPTICS_WINDOW_BUTTON_RIGHT);
    OT_EXPECT(optics_window_pointer_button_index(OPTICS_WINDOW_POINTER_BUTTON_MIDDLE) ==
              OPTICS_WINDOW_BUTTON_MIDDLE);
    /* Unknown / native-only buttons have no ui slot; backends drop. */
    OT_EXPECT(optics_window_pointer_button_index(OPTICS_WINDOW_POINTER_BUTTON_UNKNOWN) == -1);

    /* --- scroll-channel classification ------------------------------- */

    /* WL_POINTER_AXIS_SOURCE_WHEEL (0): a notched wheel — wheel steps. */
    OT_EXPECT(!optics_window_scroll_source_is_continuous(0));
    /* WL_POINTER_AXIS_SOURCE_FINGER (1): touchpad two-finger scroll —
     * continuous pixel deltas. */
    OT_EXPECT(optics_window_scroll_source_is_continuous(1));
    /* WL_POINTER_AXIS_SOURCE_CONTINUOUS (2): e.g. high-resolution wheel
     * emulation — continuous. */
    OT_EXPECT(optics_window_scroll_source_is_continuous(2));
    /* WL_POINTER_AXIS_SOURCE_WHEEL_TILT (3): tilt is a wheel-style
     * discrete gesture — steps, and its axis is horizontal anyway. */
    OT_EXPECT(!optics_window_scroll_source_is_continuous(3));

    /* The notch equivalence on the continuous channel must match the
     * ui-side consumer convention (slider.c divides scroll_pixels_y
     * by 40), or consumers folding both channels double- or half-count
     * a notch. */
    OT_EXPECT(OPTICS_WINDOW_SCROLL_PIXELS_PER_NOTCH == 40.0);

    /* --- scroll sign convention (optics_window_scroll_accum) --------------------
     *
     * Facts pinned here (cross-checked against ui consumers):
     *   - Platform events arrive positive = physical wheel-down /
     *     finger-down (wl_pointer.axis contract).
     *   - ui's contract on BOTH channels: negative = the user's down
     *     gesture (tests/ui/test_scroll.c: scroll_y = -5 reveals later
     *     content), positive = up.
     * optics_window_scroll_accum performs that single inversion at the platform
     * boundary; nothing downstream may invert again. */

    {
        double sy = 0, py = 0, sx = 0, px = 0;
        /* Wheel-down on Wayland: wl_pointer.axis vertical +10 units.
         * The caller (ptr_axis) normalises ~10 units to one notch first;
         * this helper takes the already-normalised delta. One notch of
         * down gesture -> scroll_y == -1 (down is negative in ui). */
        optics_window_scroll_accum(&sy, &py, &sx, &px, 0, 1.0, false);
        OT_EXPECT(sy == -1.0 && py == 0.0 && sx == 0.0 && px == 0.0);

        /* Wheel-up cancels it: -1 notch -> scroll_y back to 0. */
        optics_window_scroll_accum(&sy, &py, &sx, &px, 0, -1.0, false);
        OT_EXPECT(sy == 0.0 && py == 0.0);

        /* Two-finger touchpad down 25 logical px: continuous vertical.
         * Same sign inversion, pixel channel, untouched step channel. */
        double sy2 = 0, py2 = 0, sx2 = 0, px2 = 0;
        optics_window_scroll_accum(&sy2, &py2, &sx2, &px2, 0, 25.0, true);
        OT_EXPECT(sy2 == 0.0 && py2 == -25.0);

        /* Horizontal wheel tilt right (+1 notch) and touchpad right
         * (+25 px): horizontal channels, same inversion. */
        double sy3 = 0, py3 = 0, sx3 = 0, px3 = 0;
        optics_window_scroll_accum(&sy3, &py3, &sx3, &px3, 1, 1.0, false);
        OT_EXPECT(sx3 == -1.0 && px3 == 0.0 && sy3 == 0.0 && py3 == 0.0);
        optics_window_scroll_accum(&sy3, &py3, &sx3, &px3, 1, 25.0, true);
        OT_EXPECT(sx3 == -1.0 && px3 == -25.0);

        /* Accumulation is additive across events in a frame group. */
        double sy4 = 0, py4 = 0, sx4 = 0, px4 = 0;
        for (int i = 0; i < 5; i++)
            optics_window_scroll_accum(&sy4, &py4, &sx4, &px4, 0, 1.0, false);
        OT_EXPECT(sy4 == -5.0 && py4 == 0.0);
    }

    /* --- reify/apply split for the event seam (ADR-0107) ---------------
     *
     * The seam reifies a platform delta into ui space exactly once
     * (optics_window_scroll_delta), and the backend applies the surviving —
     * possibly host-rewritten — delta with optics_window_scroll_accum_delta. The
     * two must compose to the old optics_window_scroll_accum behaviour. */
    {
        OT_EXPECT(optics_window_scroll_delta(1.0) == -1.0);
        OT_EXPECT(optics_window_scroll_delta(-25.0) == 25.0);

        double sy = 0, py = 0, sx = 0, px = 0;
        optics_window_scroll_accum_delta(&sy, &py, &sx, &px, 0, -1.0, false);
        double sy2 = 0, py2 = 0, sx2 = 0, px2 = 0;
        optics_window_scroll_accum(&sy2, &py2, &sx2, &px2, 0, 1.0, false);
        OT_EXPECT(sy == sy2 && py == py2 && sy == -1.0);

        /* A host rewrite in ui space lands on the chosen channel. */
        double sy3 = 0, py3 = 0, sx3 = 0, px3 = 0;
        optics_window_scroll_accum_delta(&sy3, &py3, &sx3, &px3, 1, -25.0, true);
        OT_EXPECT(sx3 == 0.0 && px3 == -25.0);
    }

    return OT_REPORT();
}
