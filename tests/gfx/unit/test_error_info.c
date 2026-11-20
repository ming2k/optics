/*
 * Structured error slot: message lifetime.
 *
 * `flux_error_info.message` is a borrowed pointer, and the public contract in
 * <flux/core.h> says it stays readable until the next non-OK return on the same
 * thread. That contract is only true if every producer hands the slot storage
 * that outlives the producer's stack frame.
 *
 * The failure it guards against is concrete: the transient-pool budget
 * diagnostics compose a "N MiB resident, requesting WxH" line with snprintf
 * into a local `char why[192]`, then publish it. The slot kept the pointer, the
 * frame died, and `flux_get_last_error` handed the caller a dangling read —
 * visible as garbage text or a use-after-scope ASan fault, never as a failed
 * assertion elsewhere. FLUX_FAILF renders into the slot's own thread-local
 * buffer instead.
 *
 * `result.c` has hidden visibility in libflux.so, so the test compiles it in
 * directly (same precedent as test_canvas_tess / test_canvas_flatten).
 */
#include "../../../libs/gfx/src/core/internal.h"
#include "optics_test.h"

#include <stdio.h>
#include <string.h>

/* Simulates the producer shape that the defect lived in: compose a
 * value-carrying message inside a helper, publish it, return. Once this frame
 * is gone the only thing keeping the text alive is the error slot itself. */
static optics_gfx_result fail_from_nested_frame(unsigned budget_mib, unsigned width,
                                                unsigned height) {
    char local_scratch[192];
    snprintf(local_scratch, sizeof(local_scratch), "%u MiB budget, requesting %ux%u", budget_mib,
             width, height);
    /* local_scratch is deliberately not used: the point is that the published
     * message must not alias caller stack storage. */
    OPTICS_GFX_FAILF(OPTICS_GFX_ERROR_OUT_OF_MEMORY,
                     "output pool at its %u MiB budget: requesting %ux%u, every evictable entry in "
                     "flight",
                     budget_mib, width, height);
    return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
}

/* Poison the stack region the producer frame occupied, so an implementation
 * that borrowed its buffer reads back as garbage rather than luck. */
static void clobber_stack(void) {
    volatile char wash[512];
    for (size_t i = 0; i < sizeof(wash); ++i)
        wash[i] = (char)0xA5;
}

int main(void) {
    /* --- formatted message survives the producing frame --- */
    {
        OT_EXPECT(fail_from_nested_frame(256u, 3840u, 2160u) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        clobber_stack();

        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        OT_EXPECT(info.message != nullptr);
        if (info.message) {
            OT_EXPECT(strcmp(info.message,
                             "output pool at its 256 MiB budget: requesting 3840x2160, every "
                             "evictable entry in flight") == 0);
        }
        /* Provenance is captured, not just the text. */
        OT_EXPECT(info.function != nullptr);
        OT_EXPECT(info.file != nullptr);
        OT_EXPECT(info.line > 0);
        OT_EXPECT(info.backend_code == 0);
    }

    /* --- a literal message keeps pointing at the literal --- */
    {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "literal detail");
        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(info.message != nullptr);
        OT_EXPECT(info.message && strcmp(info.message, "literal detail") == 0);
    }

    /* --- the latest failure wins, and the earlier text is allowed to go --- */
    {
        OT_EXPECT(fail_from_nested_frame(64u, 128u, 128u) == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        optics_gfx_error_info first;
        optics_gfx_get_last_error(&first);
        OT_EXPECT(first.message && strstr(first.message, "128x128") != nullptr);

        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED, "second failure");
        optics_gfx_error_info second;
        optics_gfx_get_last_error(&second);
        OT_EXPECT(second.code == OPTICS_GFX_ERROR_UNSUPPORTED);
        OT_EXPECT(second.message && strcmp(second.message, "second failure") == 0);
    }

    /* --- an oversized composition truncates instead of overrunning --- */
    {
        /* 600 'x' characters; the slot holds 512 bytes including the NUL. */
        static const char long_text[] =
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        OPTICS_GFX_FAILF(OPTICS_GFX_ERROR_OUT_OF_RANGE, "%s", long_text);
        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_OUT_OF_RANGE);
        OT_EXPECT(info.message != nullptr);
        OT_EXPECT(info.message && strlen(info.message) == 511u);
        OT_EXPECT(info.message && strlen(info.message) < sizeof(long_text) - 1u);
    }

    return OT_REPORT();
}
