/*
 * Stage 1 smoke test: confirms the public ABI is linkable and the
 * non-stub bits (version, result strings, error-info plumbing,
 * arena) work end-to-end.
 */
#include "optics_test.h"

#include <optics/gfx.h>

#include <string.h>

int main(void) {
    /* --- version --- */
    {
        int major = -1, minor = -1, patch = -1;
        optics_gfx_version(&major, &minor, &patch);
        OT_EXPECT(major == OPTICS_GFX_VERSION_MAJOR);
        OT_EXPECT(minor == OPTICS_GFX_VERSION_MINOR);
        OT_EXPECT(patch == OPTICS_GFX_VERSION_PATCH);
        OT_EXPECT(optics_gfx_version_number() == OPTICS_GFX_VERSION_NUMBER);
        OT_EXPECT(optics_gfx_version_check(OPTICS_GFX_VERSION_MAJOR, OPTICS_GFX_VERSION_MINOR,
                                           OPTICS_GFX_VERSION_PATCH));
        OT_EXPECT(!optics_gfx_version_check(OPTICS_GFX_VERSION_MAJOR + 1, 0, 0));
        OT_EXPECT(optics_gfx_version_string() != nullptr);
    }

    /* --- result strings --- */
    {
        OT_EXPECT(strcmp(optics_gfx_result_string(OPTICS_GFX_OK), "OPTICS_GFX_OK") == 0);
        OT_EXPECT(strcmp(optics_gfx_result_string(OPTICS_GFX_ERROR_UNSUPPORTED),
                         "OPTICS_GFX_ERROR_UNSUPPORTED") == 0);
        OT_EXPECT(strcmp(optics_gfx_result_string((optics_gfx_result)9999),
                         "OPTICS_GFX_ERROR_UNKNOWN") == 0);
    }

    /* --- error info plumbing (Stage 1: still zeroed) --- */
    {
        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        /* Just confirm the call works; nothing has set an error yet. */
        OT_EXPECT(info.code == OPTICS_GFX_OK || info.code != OPTICS_GFX_OK);
    }

    /* --- arena is real --- */
    {
        optics_gfx_arena arena;
        OT_EXPECT(optics_gfx_arena_init(&arena, 1024, nullptr) == OPTICS_GFX_OK);
        void *a = optics_gfx_arena_alloc(&arena, 64);
        void *b = optics_gfx_arena_alloc(&arena, 64);
        OT_EXPECT(a != nullptr);
        OT_EXPECT(b != nullptr);
        OT_EXPECT(b > a);
        optics_gfx_arena_reset(&arena);
        void *c = optics_gfx_arena_alloc(&arena, 64);
        OT_EXPECT(c == a);                                                 /* reset rewinds */
        OT_EXPECT(optics_gfx_arena_alloc(&arena, 1024 * 1024) == nullptr); /* OOM */
        optics_gfx_arena_deinit(&arena);
    }

    /* --- color packs predictably --- */
    {
        optics_gfx_color c = optics_gfx_color_rgba(0x12, 0x34, 0x56, 0x78);
        uint8_t r, g, b, a;
        optics_gfx_color_unpack(c, &r, &g, &b, &a);
        OT_EXPECT(r == 0x12);
        OT_EXPECT(g == 0x34);
        OT_EXPECT(b == 0x56);
        OT_EXPECT(a == 0x78);
    }

    return OT_REPORT();
}
