/*
 * Test canvas path builder.
 */
#include "../../../libs/gfx/src/canvas/internal.h"
#include "optics_test.h"
#include <optics/gfx.h>

int main(void) {
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 65536, nullptr) == OPTICS_GFX_OK);

    /* --- rect --- */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);
        optics_gfx_path_add_rect(p, (optics_gfx_rect){10.0f, 20.0f, 30.0f, 40.0f});
        OT_EXPECT(p->count == 5);
        OT_EXPECT(p->segments[0].op == OPTICS_GFX_PATH_MOVE);
        OT_EXPECT(p->segments[p->count - 1].op == OPTICS_GFX_PATH_CLOSE);
        OT_EXPECT(optics_gfx_path_dropped_count(p) == 0);
        optics_gfx_arena_reset(&arena);
    }

    /* --- circle --- */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);
        optics_gfx_path_add_circle(p, 0.0f, 0.0f, 5.0f);
        OT_EXPECT(p->count > 0);
        OT_EXPECT(p->segments[0].op == OPTICS_GFX_PATH_MOVE);
        OT_EXPECT(p->segments[p->count - 1].op == OPTICS_GFX_PATH_CLOSE);
        optics_gfx_arena_reset(&arena);
    }

    /* --- round rect (radius > 0) --- */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);
        optics_gfx_path_add_round_rect(p, (optics_gfx_rect){0.0f, 0.0f, 10.0f, 10.0f}, 2.0f);
        OT_EXPECT(p->count > 0);
        optics_gfx_arena_reset(&arena);
    }

    /* --- round rect fallback when radius <= 0 --- */
    {
        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);
        optics_gfx_path_add_round_rect(p, (optics_gfx_rect){0.0f, 0.0f, 10.0f, 10.0f}, 0.0f);
        OT_EXPECT(p->count == 5);
        optics_gfx_arena_reset(&arena);
    }

    /* --- arena exhaustion: path grows dynamically from arena;
     * when the arena runs out, further segments are dropped. */
    {
        optics_gfx_arena tiny;
        OT_EXPECT(optics_gfx_arena_init(&tiny, 512, nullptr) == OPTICS_GFX_OK);

        optics_gfx_path *p = nullptr;
        OT_EXPECT(optics_gfx_path_create(&p, &tiny) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);

        uint32_t pushed = 0;
        for (uint32_t i = 0; i < 2000; ++i) {
            optics_gfx_path_line_to(p, (float)i, (float)i);
            pushed++;
            if (optics_gfx_path_dropped_count(p) > 0)
                break;
        }
        OT_EXPECT(optics_gfx_path_dropped_count(p) > 0);
        OT_EXPECT(p->count < pushed);

        optics_gfx_error_info info;
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_OUT_OF_MEMORY);

        OT_EXPECT(optics_gfx_path_dropped_count(nullptr) == 0);

        optics_gfx_arena_deinit(&tiny);
    }

    /* --- optics_gfx_path_create rejects NULL arena --- */
    {
        optics_gfx_path *p = (optics_gfx_path *)(uintptr_t)0xdeadbeef;
        OT_EXPECT(optics_gfx_path_create(&p, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(p == nullptr);
        OT_EXPECT(optics_gfx_path_create(nullptr, &arena) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    optics_gfx_arena_deinit(&arena);
    return OT_REPORT();
}
