/* fuzz_ui_store_model.c — model-checked fuzzing of the ui retained store.
 *
 * Complements fuzz_ui_store.c (public API, crash-only): this harness
 * compiles libs/ui/src/store/store.c directly and checks every frame
 * against the reference model in ../store_model.h — structural
 * invariants, model agreement, retained-pointer stability, hysteresis,
 * and proportionate capacity. Any violation aborts; a store loop that
 * never terminates trips libFuzzer's -timeout.
 *
 * Input: a byte program. Each op byte selects (op % 6), followed by its
 * operands (missing operands read as 0):
 *
 *   0  end frame                                   (no operands)
 *   1  touch one key                               key lo, key hi
 *   2  one frame touching a key range              first/64, count/8 - 1
 *   3  hold the last frame's keys for N frames     N/4
 *   4  hold every k-th of the last frame's keys    N/2, k - 2
 *   5  idle frames (everything starts leaving)     N % 32
 *
 * Holds are what make the shrink path reachable: the 300-frame dwell
 * needs a population that survives across frames, which the public-API
 * harness never builds (it mints a new id for every widget). Work per
 * input is capped so the fuzzer keeps a useful exec rate. */

#include "../store_model.h"

/* ~1 s per input under ASan + coverage on a CI core. Enough for several
 * shrink dwells at a few hundred live nodes, which is where the bugs are. */
#define FUZZ_MAX_FRAMES 20000u
#define FUZZ_MAX_TOUCHES 1500000u

typedef struct {
    const uint8_t *data;
    size_t size, at;
} reader;

static uint32_t next(reader *r) {
    return r->at < r->size ? r->data[r->at++] : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sm_ctx *c = sm_create(64);
    c->touch_budget = FUZZ_MAX_TOUCHES;
    reader r = {data, size, 0};
    while (r.at < r.size && c->frames < FUZZ_MAX_FRAMES && !sm_over_budget(c)) {
        switch (next(&r) % 6) {
        case 0:
            sm_end_frame(c);
            break;
        case 1: {
            uint32_t lo = next(&r), hi = next(&r);
            sm_touch(c, lo | hi << 8);
            break;
        }
        case 2: {
            uint32_t first = next(&r) * 64, count = (next(&r) + 1) * 8;
            sm_frame_range(c, first, count);
            break;
        }
        case 3:
            sm_hold(c, next(&r) * 4, 1);
            break;
        case 4: {
            uint32_t frames = next(&r) * 2, stride = next(&r) % 7 + 2;
            sm_hold(c, frames, stride);
            break;
        }
        default:
            for (uint32_t f = next(&r) % 32; f; f--)
                sm_end_frame(c);
            break;
        }
    }
    sm_end_frame(c);
    sm_destroy(c);
    return 0;
}
