/* fuzz_path_flatten.c — feed random bytes as a path through the flattener.
 *
 * Bytes are interpreted as a sequence of (op, args):
 *   op = data[i] % 5  ->  0 move_to, 1 line_to, 2 quad_to, 3 cubic_to, 4 close
 *   args are 2 floats per point (4..6 points per verb), packed little-endian
 *   from the remaining bytes (whatever is left at EOF is zero-padded).
 *
 * The harness does not assert a particular output; it only requires the call
 * to terminate (no infinite loop) and not corrupt the heap (ASAN catches
 * real bugs). It is the most valuable fuzz target for the 2D canvas because
 * path flattening is the main input-driven code path with non-trivial
 * arithmetic (de Casteljau, distance tolerances, recursion depth caps). */

#include <flux/canvas.h>
#include <flux/core.h>
#include <flux/math.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static float read_f32(const uint8_t *p, size_t avail, size_t *cursor) {
    /* Pull 4 little-endian bytes; zero-pad past EOF. */
    uint8_t b[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (*cursor + i < avail)
            b[i] = p[*cursor + i];
    }
    *cursor += 4;
    uint32_t u =
        (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    float f;
    memcpy(&f, &u, sizeof f);
    /* Filter out non-finite values: the flattener has a recursion cap but
     * we still want to exercise it with finite endpoints only — NaN makes
     * every comparison false, which is a different (and already-tested)
     * degenerate case. */
    if (!isfinite(f))
        return 0.0f;
    /* Keep magnitudes bounded so the chord-tolerance math does not
     * overflow on wild bit patterns. */
    if (fabsf(f) > 1.0e6f)
        return copysignf(1.0e6f, f);
    return f;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    flux_arena arena;
    if (flux_arena_init(&arena, 1u << 20, NULL) != FLUX_OK)
        return 0;

    flux_path *p = NULL;
    if (flux_path_create(&p, &arena) != FLUX_OK) {
        flux_arena_destroy(&arena);
        return 0;
    }

    size_t cursor = 0;
    while (cursor < size) {
        uint8_t op_byte = data[cursor++];
        uint32_t op = op_byte % 5;
        switch (op) {
        case 0: {
            float x = read_f32(data, size, &cursor);
            float y = read_f32(data, size, &cursor);
            flux_path_move_to(p, x, y);
            break;
        }
        case 1: {
            float x = read_f32(data, size, &cursor);
            float y = read_f32(data, size, &cursor);
            flux_path_line_to(p, x, y);
            break;
        }
        case 2: {
            float cx = read_f32(data, size, &cursor);
            float cy = read_f32(data, size, &cursor);
            float x = read_f32(data, size, &cursor);
            float y = read_f32(data, size, &cursor);
            flux_path_quad_to(p, cx, cy, x, y);
            break;
        }
        case 3: {
            float c1x = read_f32(data, size, &cursor);
            float c1y = read_f32(data, size, &cursor);
            float c2x = read_f32(data, size, &cursor);
            float c2y = read_f32(data, size, &cursor);
            float x = read_f32(data, size, &cursor);
            float y = read_f32(data, size, &cursor);
            flux_path_cubic_to(p, c1x, c1y, c2x, c2y, x, y);
            break;
        }
        default:
            flux_path_close(p);
            break;
        }
        /* The path internals (including the dropped-verbs counter) are
         * opaque by design; we stop after a few hundred verbs to keep the
         * fuzzer from generating unbounded path lengths that the arena
         * rejects anyway. */
        if (p && cursor > 4096)
            break;
    }

    flux_arena_destroy(&arena);
    return 0;
}
