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
#include "../../../libs/flux/src/core/internal.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

/* Simulates the producer shape that the defect lived in: compose a
 * value-carrying message inside a helper, publish it, return. Once this frame
 * is gone the only thing keeping the text alive is the error slot itself. */
static flux_result fail_from_nested_frame(unsigned budget_mib, unsigned width, unsigned height) {
    char local_scratch[192];
    snprintf(local_scratch, sizeof(local_scratch), "%u MiB budget, requesting %ux%u", budget_mib,
             width, height);
    /* local_scratch is deliberately not used: the point is that the published
     * message must not alias caller stack storage. */
    FLUX_FAILF(FLUX_ERROR_OUT_OF_MEMORY,
               "output pool at its %u MiB budget: requesting %ux%u, every evictable entry in "
               "flight",
               budget_mib, width, height);
    return FLUX_ERROR_OUT_OF_MEMORY;
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
        EXPECT(fail_from_nested_frame(256u, 3840u, 2160u) == FLUX_ERROR_OUT_OF_MEMORY);
        clobber_stack();

        flux_error_info info;
        flux_get_last_error(&info);
        EXPECT(info.code == FLUX_ERROR_OUT_OF_MEMORY);
        EXPECT(info.message != nullptr);
        if (info.message) {
            EXPECT(strcmp(info.message,
                          "output pool at its 256 MiB budget: requesting 3840x2160, every "
                          "evictable entry in flight") == 0);
        }
        /* Provenance is captured, not just the text. */
        EXPECT(info.function != nullptr);
        EXPECT(info.file != nullptr);
        EXPECT(info.line > 0);
        EXPECT(info.backend_code == 0);
    }

    /* --- a literal message keeps pointing at the literal --- */
    {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "literal detail");
        flux_error_info info;
        flux_get_last_error(&info);
        EXPECT(info.code == FLUX_ERROR_INVALID_STATE);
        EXPECT(info.message != nullptr);
        EXPECT(info.message && strcmp(info.message, "literal detail") == 0);
    }

    /* --- the latest failure wins, and the earlier text is allowed to go --- */
    {
        EXPECT(fail_from_nested_frame(64u, 128u, 128u) == FLUX_ERROR_OUT_OF_MEMORY);
        flux_error_info first;
        flux_get_last_error(&first);
        EXPECT(first.message && strstr(first.message, "128x128") != nullptr);

        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "second failure");
        flux_error_info second;
        flux_get_last_error(&second);
        EXPECT(second.code == FLUX_ERROR_UNSUPPORTED);
        EXPECT(second.message && strcmp(second.message, "second failure") == 0);
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
        FLUX_FAILF(FLUX_ERROR_OUT_OF_RANGE, "%s", long_text);
        flux_error_info info;
        flux_get_last_error(&info);
        EXPECT(info.code == FLUX_ERROR_OUT_OF_RANGE);
        EXPECT(info.message != nullptr);
        EXPECT(info.message && strlen(info.message) == 511u);
        EXPECT(info.message && strlen(info.message) < sizeof(long_text) - 1u);
    }

    TEST_SUMMARY();
}
