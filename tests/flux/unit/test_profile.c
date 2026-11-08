/*
 * Unit tests for profiler telemetry facade (ADR-0138).
 */

#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/profile.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* --- query API --- */
    {
        flux_profile_status st = flux_profile_query(nullptr);
#if defined(OPTICS_PROFILER_TRACY)
        EXPECT(st.enabled == true);
#else
        EXPECT(st.enabled == false);
        EXPECT(st.connected == false);
#endif

        flux_profile_opts opts = {
            .app_info = "Optics Unit Test Suite",
        };
        flux_profile_status st2 = flux_profile_query(&opts);
#if defined(OPTICS_PROFILER_TRACY)
        EXPECT(st2.enabled == true);
#else
        EXPECT(st2.enabled == false);
        EXPECT(st2.connected == false);
#endif
    }

    /* --- macro verification (must compile and execute cleanly) --- */
    {
        FLUX_PROFILE_SET_THREAD_NAME("unit-test");

        FLUX_PROFILE_FRAME_START("TestFrame");
        FLUX_PROFILE_FRAME("TestFrame");
        FLUX_PROFILE_FRAME_MARK();

        FLUX_PROFILE_PLOT("test_metric", 42.5);
        FLUX_PROFILE_PLOT_INT("test_counter", 100);

        /* Unused when Tracy is compiled out: the alloc/free macros expand to
         * ((void)0), so -Wunused-variable would fail the release build. */
        [[maybe_unused]] void *dummy = (void *)(uintptr_t)0x12345000;
        FLUX_PROFILE_ALLOC(dummy, 64);
        FLUX_PROFILE_FREE(dummy);

        FLUX_PROFILE_ALLOC_NAMED(dummy, 128, "test_arena");
        FLUX_PROFILE_FREE_NAMED(dummy, "test_arena");

        FLUX_PROFILE_MESSAGE("optics profiler verification", 28);

        FLUX_PROFILE_ZONE(z1, "outer_zone");
        {
            FLUX_PROFILE_ZONE_COLOR(z2, "inner_zone", 0x00FF88u);
            volatile int sum = 0;
            for (int i = 0; i < 100; ++i)
                sum += i;
            (void)sum;
            FLUX_PROFILE_ZONE_END(z2);
        }
        FLUX_PROFILE_ZONE_END(z1);

        FLUX_PROFILE_FRAME_END("TestFrame");
    }

    return 0;
}
