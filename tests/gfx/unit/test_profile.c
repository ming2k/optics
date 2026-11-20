/*
 * Unit tests for profiler telemetry facade (ADR-0138).
 */

#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/profile.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* --- query API --- */
    {
        optics_gfx_profile_status st = optics_gfx_profile_query(nullptr);
#if defined(OPTICS_PROFILER_TRACY)
        OT_EXPECT(st.enabled == true);
#else
        OT_EXPECT(st.enabled == false);
        OT_EXPECT(st.connected == false);
#endif

        optics_gfx_profile_opts opts = {
            .app_info = "Optics Unit Test Suite",
        };
        optics_gfx_profile_status st2 = optics_gfx_profile_query(&opts);
#if defined(OPTICS_PROFILER_TRACY)
        OT_EXPECT(st2.enabled == true);
#else
        OT_EXPECT(st2.enabled == false);
        OT_EXPECT(st2.connected == false);
#endif
    }

    /* --- macro verification (must compile and execute cleanly) --- */
    {
        OPTICS_GFX_PROFILE_SET_THREAD_NAME("unit-test");

        OPTICS_GFX_PROFILE_FRAME_START("TestFrame");
        OPTICS_GFX_PROFILE_FRAME("TestFrame");
        OPTICS_GFX_PROFILE_FRAME_MARK();

        OPTICS_GFX_PROFILE_PLOT("test_metric", 42.5);
        OPTICS_GFX_PROFILE_PLOT_INT("test_counter", 100);

        /* Unused when Tracy is compiled out: the alloc/free macros expand to
         * ((void)0), so -Wunused-variable would fail the release build. */
        [[maybe_unused]] void *dummy = (void *)(uintptr_t)0x12345000;
        OPTICS_GFX_PROFILE_ALLOC(dummy, 64);
        OPTICS_GFX_PROFILE_FREE(dummy);

        OPTICS_GFX_PROFILE_ALLOC_NAMED(dummy, 128, "test_arena");
        OPTICS_GFX_PROFILE_FREE_NAMED(dummy, "test_arena");

        OPTICS_GFX_PROFILE_MESSAGE("optics profiler verification", 28);

        OPTICS_GFX_PROFILE_ZONE(z1, "outer_zone");
        {
            OPTICS_GFX_PROFILE_ZONE_COLOR(z2, "inner_zone", 0x00FF88u);
            volatile int sum = 0;
            for (int i = 0; i < 100; ++i)
                sum += i;
            (void)sum;
            OPTICS_GFX_PROFILE_ZONE_END(z2);
        }
        OPTICS_GFX_PROFILE_ZONE_END(z1);

        OPTICS_GFX_PROFILE_FRAME_END("TestFrame");
    }

    return 0;
}
