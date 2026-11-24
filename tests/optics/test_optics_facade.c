/*
 * test_optics_facade.c — verification of the unified Optics C23 facade (ADR-0160).
 *
 * Verifies that:
 *   1. Including <optics/optics.h> alone provides full visibility into all engine
 *      layers without macro collisions or missing typedefs.
 *   2. Header-only motion vocabulary functions execute correctly.
 *   3. Core mathematical and geometric structures from gfx are accessible.
 *   4. L3 state engines (ui, window) types and version macros are consistent.
 *   5. Pure C23 compiler conformance.
 */

#include "optics_test.h"

#include <optics/optics.h>

int main(void) {
    /* 1. Verify Umbrella Facade Version Macros & Lockstep Invariants */
    static_assert(OPTICS_VERSION_MAJOR == 0, "Optics major version mismatch");
    static_assert(OPTICS_VERSION_MINOR == 0, "Optics minor version mismatch");
    static_assert(OPTICS_VERSION_PATCH == 86, "Optics patch version mismatch");
    static_assert(OPTICS_VERSION_NUMBER == 86, "Optics version number mismatch");

    static_assert(OPTICS_GFX_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "gfx/Optics major mismatch");
    static_assert(OPTICS_GFX_VERSION_MINOR == OPTICS_VERSION_MINOR, "gfx/Optics minor mismatch");
    static_assert(OPTICS_GFX_VERSION_PATCH == OPTICS_VERSION_PATCH, "gfx/Optics patch mismatch");

#if defined(OPTICS_TEXT_VERSION_MAJOR)
    static_assert(OPTICS_TEXT_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Glyph/Optics major mismatch");
    static_assert(OPTICS_TEXT_VERSION_PATCH == OPTICS_VERSION_PATCH, "Glyph/Optics patch mismatch");
#endif

#if defined(OPTICS_MATERIAL_VERSION_MAJOR)
    static_assert(OPTICS_MATERIAL_VERSION_MAJOR == OPTICS_VERSION_MAJOR,
                  "material/Optics major mismatch");
    static_assert(OPTICS_MATERIAL_VERSION_PATCH == OPTICS_VERSION_PATCH,
                  "material/Optics patch mismatch");
#endif

#if defined(OPTICS_SCENE_VERSION_MAJOR)
    static_assert(OPTICS_SCENE_VERSION_MAJOR == OPTICS_VERSION_MAJOR,
                  "scene/Optics major mismatch");
    static_assert(OPTICS_SCENE_VERSION_PATCH == OPTICS_VERSION_PATCH,
                  "scene/Optics patch mismatch");
#endif

#if defined(OPTICS_SVG_VERSION_MAJOR)
    static_assert(OPTICS_SVG_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "svg/Optics major mismatch");
    static_assert(OPTICS_SVG_VERSION_PATCH == OPTICS_VERSION_PATCH, "svg/Optics patch mismatch");
#endif

    static_assert(OPTICS_UI_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "ui/Optics major mismatch");
    static_assert(OPTICS_UI_VERSION_PATCH == OPTICS_VERSION_PATCH, "ui/Optics patch mismatch");

    static_assert(OPTICS_WINDOW_VERSION_MAJOR == OPTICS_VERSION_MAJOR,
                  "window/Optics major mismatch");
    static_assert(OPTICS_WINDOW_VERSION_PATCH == OPTICS_VERSION_PATCH,
                  "window/Optics patch mismatch");

    /* 2. Verify Layer 0 (gfx) structures and versioning */
    optics_gfx_vec2 v = {.x = 10.0f, .y = 20.0f};
    OT_EXPECT(v.x == 10.0f && v.y == 20.0f);

    optics_gfx_rect r = {.x = 0.0f, .y = 0.0f, .w = 1920.0f, .h = 1080.0f};
    OT_EXPECT(r.w == 1920.0f && r.h == 1080.0f);

    /* 3. Verify the header-only motion vocabulary */
    optics_motion_spring spring = {.value = 0.0f, .velocity = 0.0f};
    optics_motion_spring_params params = {.stiffness = 180.0f, .damping = 12.0f};
    float next_val = optics_motion_spring_advance(&spring, 1.0f, params, 0.016f, false);
    OT_EXPECT(next_val > 0.0f && next_val <= 1.0f);

    float eased = optics_motion_ease_in_out_cubic(0.5f);
    OT_EXPECT(eased == 0.5f);

    /* 4. Verify Layer 3 (ui & window) definitions and versions */
    static_assert(OPTICS_UI_VERSION_MAJOR == 0, "ui major version mismatch");
    static_assert(OPTICS_WINDOW_VERSION_MAJOR == 0, "window major version mismatch");

    int optics_window_maj = 0;
    int optics_window_min = 0;
    int optics_window_pat = 0;
    optics_window_version(&optics_window_maj, &optics_window_min, &optics_window_pat);
    OT_EXPECT(optics_window_maj == OPTICS_VERSION_MAJOR &&
              optics_window_min == OPTICS_VERSION_MINOR &&
              optics_window_pat == OPTICS_VERSION_PATCH);

    /* Diagnostic log must strictly go to stderr (Library Stdout Purity) */
    fprintf(stderr, "test_optics_facade: all umbrella facade checks passed.\n");
    return OT_REPORT();
}
