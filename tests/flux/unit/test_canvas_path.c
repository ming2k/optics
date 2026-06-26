/*
 * Test canvas path builder.
 */
#include "../../../libs/flux/src/canvas/internal.h"
#include "test_helpers.h"
#include <flux/flux.h>

int main(void) {
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 65536, nullptr) == FLUX_OK);

    /* --- rect --- */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        EXPECT(p != nullptr);
        flux_path_add_rect(p, (flux_rect){10.0f, 20.0f, 30.0f, 40.0f});
        EXPECT(p->count == 5);
        EXPECT(p->segments[0].op == FLUX_PATH_MOVE);
        EXPECT(p->segments[p->count - 1].op == FLUX_PATH_CLOSE);
        EXPECT(flux_path_dropped_count(p) == 0);
        flux_arena_reset(&arena);
    }

    /* --- circle --- */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        EXPECT(p != nullptr);
        flux_path_add_circle(p, 0.0f, 0.0f, 5.0f);
        EXPECT(p->count > 0);
        EXPECT(p->segments[0].op == FLUX_PATH_MOVE);
        EXPECT(p->segments[p->count - 1].op == FLUX_PATH_CLOSE);
        flux_arena_reset(&arena);
    }

    /* --- round rect (radius > 0) --- */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        EXPECT(p != nullptr);
        flux_path_add_round_rect(p, (flux_rect){0.0f, 0.0f, 10.0f, 10.0f}, 2.0f);
        EXPECT(p->count > 0);
        flux_arena_reset(&arena);
    }

    /* --- round rect fallback when radius <= 0 --- */
    {
        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &arena) == FLUX_OK);
        EXPECT(p != nullptr);
        flux_path_add_round_rect(p, (flux_rect){0.0f, 0.0f, 10.0f, 10.0f}, 0.0f);
        EXPECT(p->count == 5);
        flux_arena_reset(&arena);
    }

    /* --- arena exhaustion: path grows dynamically from arena;
     * when the arena runs out, further segments are dropped. */
    {
        flux_arena tiny;
        EXPECT(flux_arena_init(&tiny, 512, nullptr) == FLUX_OK);

        flux_path *p = nullptr;
        EXPECT(flux_path_create(&p, &tiny) == FLUX_OK);
        EXPECT(p != nullptr);

        uint32_t pushed = 0;
        for (uint32_t i = 0; i < 2000; ++i) {
            flux_path_line_to(p, (float)i, (float)i);
            pushed++;
            if (flux_path_dropped_count(p) > 0)
                break;
        }
        EXPECT(flux_path_dropped_count(p) > 0);
        EXPECT(p->count < pushed);

        flux_error_info info;
        flux_get_last_error(&info);
        EXPECT(info.code == FLUX_ERROR_OUT_OF_MEMORY);

        EXPECT(flux_path_dropped_count(nullptr) == 0);

        flux_arena_destroy(&tiny);
    }

    /* --- flux_path_create rejects NULL arena --- */
    {
        flux_path *p = (flux_path *)(uintptr_t)0xdeadbeef;
        EXPECT(flux_path_create(&p, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(p == nullptr);
        EXPECT(flux_path_create(nullptr, &arena) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    flux_arena_destroy(&arena);
    TEST_SUMMARY();
}
