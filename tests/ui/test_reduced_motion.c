/* test_reduced_motion.c — optics_ui_set_reduced_motion resolves eases in one frame. */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_reduced_motion_getter_roundtrip(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    OT_EXPECT(!optics_ui_reduced_motion(ui));
    optics_ui_set_reduced_motion(ui, true);
    OT_EXPECT(optics_ui_reduced_motion(ui));
    optics_ui_set_reduced_motion(ui, false);
    OT_EXPECT(!optics_ui_reduced_motion(ui));
    /* NULL-safety, matching the other accessors. */
    OT_EXPECT(!optics_ui_reduced_motion(nullptr));
    optics_ui_set_reduced_motion(nullptr, true);
    optics_ui_release(ui);
}

static void test_reduced_motion_approach_snaps_to_target(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    float eased = optics_ui__approach(ui, 0.0f, 1.0f, 0.016f, 12.0f);
    OT_EXPECT(eased < 1.0f);
    OT_EXPECT(optics_ui_anim_pending(ui));

    ui->anim_pending = false;
    optics_ui_set_reduced_motion(ui, true);
    eased = optics_ui__approach(ui, 0.0f, 1.0f, 0.016f, 12.0f);
    OT_EXPECT_NEAR(eased, 1.0f, 0.0001f);
    OT_EXPECT(!optics_ui_anim_pending(ui));

    optics_ui_release(ui);
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
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Drive the ease for longer than any real animation lasts at 60 Hz. */
    float value = 0.0f;
    int frames_until_settled = -1;
    for (int i = 0; i < 600; i++) {
        value = optics_ui__approach(ui, value, 1.0f, 1.0f / 60.0f, 18.0f);
        if (value == 1.0f) {
            frames_until_settled = i;
            break;
        }
    }
    OT_EXPECT(frames_until_settled >= 0); /* it terminates at all */
    OT_EXPECT(value == 1.0f);             /* exactly, not approximately */

    /* And after settling it reports no motion, which is what lets the frame be
     * declared static. */
    ui->anim_pending = false;
    for (int i = 0; i < 10; i++) {
        value = optics_ui__approach(ui, value, 1.0f, 1.0f / 60.0f, 18.0f);
        OT_EXPECT(value == 1.0f);
    }
    OT_EXPECT(!optics_ui_anim_pending(ui));

    /* A settled value also has zero delta from frame to frame, so a node's
     * `hover_t != last_hover_t` change test stops firing. That equality is the
     * whole point: `optics_ui__approach` must be idempotent on its own output. */
    OT_EXPECT(optics_ui__approach(ui, 1.0f, 1.0f, 1.0f / 60.0f, 18.0f) == 1.0f);

    optics_ui_release(ui);
}

static void test_reduced_motion_hover_stays_settled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_set_reduced_motion(ui, true);
    float value = 0.5f;

    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_anim_pending(ui));

    /* The same hover that animates the knob in test_slider must not. */
    optics_ui_input hover = IN0;
    hover.cursor = (optics_gfx_point){80, 16};
    optics_ui_begin(ui, &hover);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_anim_pending(ui));

    optics_ui_release(ui);
}

int main(void) {
    test_reduced_motion_getter_roundtrip();
    test_reduced_motion_approach_snaps_to_target();
    test_approach_terminates_instead_of_converging();
    test_reduced_motion_hover_stays_settled();
    return 0;
}
