#include <flux/profile.h>
#include <string.h>

flux_profile_status flux_profile_query(const flux_profile_opts *opts) {
    flux_profile_status st = {
#if defined(OPTICS_PROFILER_TRACY)
        .enabled = true,
        .connected = (___tracy_connected() != 0),
#else
        .enabled = false,
        .connected = false,
#endif
    };

    if (opts && opts->app_info) {
        FLUX_PROFILE_APP_INFO(opts->app_info, strlen(opts->app_info));
    }

    return st;
}
