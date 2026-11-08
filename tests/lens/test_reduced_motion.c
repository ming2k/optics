/* test_reduced_motion.c — lens_set_reduced_motion resolves eases in one frame. */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_reduced_motion_getter_roundtrip(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK(!lens_reduced_motion(ui));
    lens_set_reduced_motion(ui, true);
    CHECK(lens_reduced_motion(ui));
    lens_set_reduced_motion(ui, false);
    CHECK(!lens_reduced_motion(ui));
    /* NULL-safety, matching the other accessors. */
    CHECK(!lens_reduced_motion(NULL));
    lens_set_reduced_motion(NULL, true);
    lens_release(ui);
}

static void test_reduced_motion_approach_snaps_to_target(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    float eased = lensi_approach(ui, 0.0f, 1.0f, 0.016f, 12.0f);
    CHECK(eased < 1.0f);
    CHECK(lens_anim_pending(ui));

    ui->anim_pending = false;
    lens_set_reduced_motion(ui, true);
    eased = lensi_approach(ui, 0.0f, 1.0f, 0.016f, 12.0f);
    CHECK_NEAR(eased, 1.0f, 0.0001f);
    CHECK(!lens_anim_pending(ui));

    lens_release(ui);
}

/* An ease must terminate, not merely converge.
 *
 * A pure exponential only reaches its target at infinity, and the final float
 * ulps are indistinguishable from motion to everything downstream: a node whose
 * `hover_t` still moves by 1e-7 marks itself changed, bumps its visual
 * revision, emits damage, keeps the pacer's active cadence alive, and holds the
 * backend subscribed to the display pulse train — forever, with no pixel ever
 * different. Measured before the snap existed: a static window with the pointer
 * untouched held the client at ~70 wakeups/second.
 *
 * So the property is not "gets close"; it is "becomes exactly equal, and stops
 * reporting motion". */
static void test_approach_terminates_instead_of_converging(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Drive the ease for longer than any real animation lasts at 60 Hz. */
    float value = 0.0f;
    int frames_until_settled = -1;
    for (int i = 0; i < 600; i++) {
        value = lensi_approach(ui, value, 1.0f, 1.0f / 60.0f, 18.0f);
        if (value == 1.0f) {
            frames_until_settled = i;
            break;
        }
    }
    CHECK(frames_until_settled >= 0); /* it terminates at all */
    CHECK(value == 1.0f);             /* exactly, not approximately */

    /* And after settling it reports no motion, which is what lets the frame be
     * declared static. */
    ui->anim_pending = false;
    for (int i = 0; i < 10; i++) {
        value = lensi_approach(ui, value, 1.0f, 1.0f / 60.0f, 18.0f);
        CHECK(value == 1.0f);
    }
    CHECK(!lens_anim_pending(ui));

    /* A settled value also has zero delta from frame to frame, so a node's
     * `hover_t != last_hover_t` change test stops firing. That equality is the
     * whole point: `lensi_approach` must be idempotent on its own output. */
    CHECK(lensi_approach(ui, 1.0f, 1.0f, 1.0f / 60.0f, 18.0f) == 1.0f);

    lens_release(ui);
}

static void test_reduced_motion_hover_stays_settled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_reduced_motion(ui, true);
    float value = 0.5f;

    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    test_end(ui);
    CHECK(!lens_anim_pending(ui));

    /* The same hover that animates the knob in test_slider must not. */
    lens_input hover = IN0;
    hover.cursor = (flux_point){80, 16};
    lens_begin(ui, &hover);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    test_end(ui);
    CHECK(!lens_anim_pending(ui));

    lens_release(ui);
}

int main(void) {
    test_reduced_motion_getter_roundtrip();
    test_reduced_motion_approach_snaps_to_target();
    test_approach_terminates_instead_of_converging();
    test_reduced_motion_hover_stays_settled();
    return 0;
}
