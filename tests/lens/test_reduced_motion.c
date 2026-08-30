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
    lens_destroy(ui);
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

    lens_destroy(ui);
}

static void test_reduced_motion_hover_stays_settled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_reduced_motion(ui, true);
    float value = 0.5f;

    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_end(ui);
    CHECK(!lens_anim_pending(ui));

    /* The same hover that animates the knob in test_slider must not. */
    lens_input hover = IN0;
    hover.cursor = (flux_point){80, 16};
    lens_begin(ui, &hover);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_end(ui);
    CHECK(!lens_anim_pending(ui));

    lens_destroy(ui);
}

int main(void) {
    test_reduced_motion_getter_roundtrip();
    test_reduced_motion_approach_snaps_to_target();
    test_reduced_motion_hover_stays_settled();
    return 0;
}
