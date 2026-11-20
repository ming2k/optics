/*
 * Stage 1: minimal-but-real arena. Stage 3 may add alignment helpers,
 * scratch-pages, etc.
 */
#include <optics/gfx/math.h>
#include <optics/gfx/profile.h>
#include <stdlib.h>
#include <string.h>

static void *libc_alloc(size_t n, void *u) {
    (void)u;
    return malloc(n);
}
static void libc_free(void *p, void *u) {
    (void)u;
    free(p);
}

/* `realloc` is not part of flux_allocator (ADR-0135): no library called it, so
 * its semantics were undefined by construction. The arena is a bump allocator
 * and never grows; it allocates once and frees once. */
static const optics_gfx_allocator libc_allocator = {
    .alloc = libc_alloc,
    .free = libc_free,
    .user = nullptr,
};

optics_gfx_result optics_gfx_arena_init(optics_gfx_arena *a, size_t capacity,
                                        const optics_gfx_allocator *alloc) {
    if (!a || capacity == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    a->alloc = alloc ? *alloc : libc_allocator;
    a->base = a->alloc.alloc(capacity, a->alloc.user);
    if (!a->base)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    a->capacity = capacity;
    a->used = 0;
    a->owns_buffer = true;
    return OPTICS_GFX_OK;
}

void optics_gfx_arena_deinit(optics_gfx_arena *a) {
    if (!a)
        return;
    if (a->owns_buffer && a->base && a->alloc.free)
        a->alloc.free(a->base, a->alloc.user);
    *a = (optics_gfx_arena){};
}

void *optics_gfx_arena_alloc(optics_gfx_arena *a, size_t bytes) {
    return optics_gfx_arena_alloc_aligned(a, bytes, alignof(max_align_t));
}

void *optics_gfx_arena_alloc_aligned(optics_gfx_arena *a, size_t bytes, size_t align) {
    if (!a || !a->base || align == 0)
        return nullptr;
    /* Align against the absolute address, not the in-arena offset: the
     * base pointer may itself be unaligned to the requested boundary. */
    uintptr_t base_addr = (uintptr_t)a->base;
    uintptr_t cur_addr = base_addr + (uintptr_t)a->used;
    uintptr_t mask = (uintptr_t)align - 1;
    uintptr_t aligned_addr = (cur_addr + mask) & ~mask;
    size_t new_used = (size_t)(aligned_addr - base_addr) + bytes;
    if (new_used > a->capacity)
        return nullptr;
    a->used = new_used;
    return (void *)aligned_addr;
}

void optics_gfx_arena_reset(optics_gfx_arena *a) {
    if (a) {
        OPTICS_GFX_PROFILE_PLOT("flux.arena_used_bytes", (double)a->used);
        a->used = 0;
    }
}
