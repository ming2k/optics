#include <optics/gfx/profile.h>
#include <string.h>

optics_gfx_profile_status optics_gfx_profile_query(const optics_gfx_profile_opts *opts) {
    optics_gfx_profile_status st = {
#if defined(OPTICS_PROFILER_TRACY)
        .enabled = true,
        .connected = (___tracy_connected() != 0),
#else
        .enabled = false,
        .connected = false,
#endif
    };

    if (opts && opts->app_info) {
        OPTICS_GFX_PROFILE_APP_INFO(opts->app_info, strlen(opts->app_info));
    }

    return st;
}
