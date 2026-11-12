/*
 * test_optics_facade.c — verification of the unified Optics C23 facade (ADR-0160).
 *
 * Verifies that:
 *   1. Including <optics/optics.h> alone provides full visibility into all engine
 *      layers without macro collisions or missing typedefs.
 *   2. Header-only motion vocabulary (transit) functions execute correctly.
 *   3. Core mathematical and geometric structures from flux are accessible.
 *   4. L3 state engines (lens, iris) types and version macros are consistent.
 *   5. Pure C23 compiler conformance.
 */

#include <optics/optics.h>
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* 1. Verify Umbrella Facade Version Macros & Lockstep Invariants */
    static_assert(OPTICS_VERSION_MAJOR == 0, "Optics major version mismatch");
    static_assert(OPTICS_VERSION_MINOR == 0, "Optics minor version mismatch");
    static_assert(OPTICS_VERSION_PATCH == 78, "Optics patch version mismatch");
    static_assert(OPTICS_VERSION_NUMBER == 78, "Optics version number mismatch");

    static_assert(FLUX_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Flux/Optics major mismatch");
    static_assert(FLUX_VERSION_MINOR == OPTICS_VERSION_MINOR, "Flux/Optics minor mismatch");
    static_assert(FLUX_VERSION_PATCH == OPTICS_VERSION_PATCH, "Flux/Optics patch mismatch");

#if defined(GLYPH_VERSION_MAJOR)
    static_assert(GLYPH_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Glyph/Optics major mismatch");
    static_assert(GLYPH_VERSION_PATCH == OPTICS_VERSION_PATCH, "Glyph/Optics patch mismatch");
#endif

#if defined(PRISM_VERSION_MAJOR)
    static_assert(PRISM_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Prism/Optics major mismatch");
    static_assert(PRISM_VERSION_PATCH == OPTICS_VERSION_PATCH, "Prism/Optics patch mismatch");
#endif

#if defined(VISTA_VERSION_MAJOR)
    static_assert(VISTA_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Vista/Optics major mismatch");
    static_assert(VISTA_VERSION_PATCH == OPTICS_VERSION_PATCH, "Vista/Optics patch mismatch");
#endif

#if defined(RETICLE_VERSION_MAJOR)
    static_assert(RETICLE_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Reticle/Optics major mismatch");
    static_assert(RETICLE_VERSION_PATCH == OPTICS_VERSION_PATCH, "Reticle/Optics patch mismatch");
#endif

    static_assert(LENS_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Lens/Optics major mismatch");
    static_assert(LENS_VERSION_PATCH == OPTICS_VERSION_PATCH, "Lens/Optics patch mismatch");

    static_assert(IRIS_VERSION_MAJOR == OPTICS_VERSION_MAJOR, "Iris/Optics major mismatch");
    static_assert(IRIS_VERSION_PATCH == OPTICS_VERSION_PATCH, "Iris/Optics patch mismatch");

    /* 2. Verify Layer 0 (Flux) structures and versioning */
    flux_vec2 v = {.x = 10.0f, .y = 20.0f};
    assert(v.x == 10.0f && v.y == 20.0f);

    flux_rect r = {.x = 0.0f, .y = 0.0f, .w = 1920.0f, .h = 1080.0f};
    assert(r.w == 1920.0f && r.h == 1080.0f);

    /* 3. Verify Layer 2 (Transit) header-only motion vocabulary */
    transit_spring spring = {.value = 0.0f, .velocity = 0.0f};
    transit_spring_params params = {.stiffness = 180.0f, .damping = 12.0f};
    float next_val = transit_spring_advance(&spring, 1.0f, params, 0.016f, false);
    assert(next_val > 0.0f && next_val <= 1.0f);

    float eased = transit_ease_in_out_cubic(0.5f);
    assert(eased == 0.5f);

    /* 4. Verify Layer 3 (Lens & Iris) definitions and versions */
    static_assert(LENS_VERSION_MAJOR == 0, "Lens major version mismatch");
    static_assert(IRIS_VERSION_MAJOR == 0, "Iris major version mismatch");

    int iris_maj = 0;
    int iris_min = 0;
    int iris_pat = 0;
    iris_version(&iris_maj, &iris_min, &iris_pat);
    assert(iris_maj == 0 && iris_min == 0 && iris_pat == 78);

    /* Diagnostic log must strictly go to stderr (Library Stdout Purity) */
    fprintf(stderr, "test_optics_facade: all umbrella facade checks passed.\n");
    return 0;
}
