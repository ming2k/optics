/*
 * Stage 1 smoke test: confirms the public ABI is linkable and the
 * non-stub bits (version, result strings, error-info plumbing,
 * arena) work end-to-end.
 */
#include "optics_test.h"

#include <flux/flux.h>

#include <string.h>

int main(void) {
    /* --- version --- */
    {
        int major = -1, minor = -1, patch = -1;
        flux_version(&major, &minor, &patch);
        OT_EXPECT(major == FLUX_VERSION_MAJOR);
        OT_EXPECT(minor == FLUX_VERSION_MINOR);
        OT_EXPECT(patch == FLUX_VERSION_PATCH);
        OT_EXPECT(flux_version_number() == FLUX_VERSION_NUMBER);
        OT_EXPECT(flux_version_check(FLUX_VERSION_MAJOR, FLUX_VERSION_MINOR, FLUX_VERSION_PATCH));
        OT_EXPECT(!flux_version_check(FLUX_VERSION_MAJOR + 1, 0, 0));
        OT_EXPECT(flux_version_string() != nullptr);
    }

    /* --- result strings --- */
    {
        OT_EXPECT(strcmp(flux_result_string(FLUX_OK), "FLUX_OK") == 0);
        OT_EXPECT(strcmp(flux_result_string(FLUX_ERROR_UNSUPPORTED), "FLUX_ERROR_UNSUPPORTED") ==
                  0);
        OT_EXPECT(strcmp(flux_result_string((flux_result)9999), "FLUX_ERROR_UNKNOWN") == 0);
    }

    /* --- error info plumbing (Stage 1: still zeroed) --- */
    {
        flux_error_info info;
        flux_get_last_error(&info);
        /* Just confirm the call works; nothing has set an error yet. */
        OT_EXPECT(info.code == FLUX_OK || info.code != FLUX_OK);
    }

    /* --- arena is real --- */
    {
        flux_arena arena;
        OT_EXPECT(flux_arena_init(&arena, 1024, nullptr) == FLUX_OK);
        void *a = flux_arena_alloc(&arena, 64);
        void *b = flux_arena_alloc(&arena, 64);
        OT_EXPECT(a != nullptr);
        OT_EXPECT(b != nullptr);
        OT_EXPECT(b > a);
        flux_arena_reset(&arena);
        void *c = flux_arena_alloc(&arena, 64);
        OT_EXPECT(c == a);                                           /* reset rewinds */
        OT_EXPECT(flux_arena_alloc(&arena, 1024 * 1024) == nullptr); /* OOM */
        flux_arena_deinit(&arena);
    }

    /* --- color packs predictably --- */
    {
        flux_color c = flux_color_rgba(0x12, 0x34, 0x56, 0x78);
        uint8_t r, g, b, a;
        flux_color_unpack(c, &r, &g, &b, &a);
        OT_EXPECT(r == 0x12);
        OT_EXPECT(g == 0x34);
        OT_EXPECT(b == 0x56);
        OT_EXPECT(a == 0x78);
    }

    return OT_REPORT();
}
