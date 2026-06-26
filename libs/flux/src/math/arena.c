/*
 * Stage 1: minimal-but-real arena. Stage 3 may add alignment helpers,
 * scratch-pages, etc.
 */
#include <flux/math.h>
#include <stdlib.h>
#include <string.h>

static void *libc_alloc(size_t n, void *u) {
    (void)u;
    return malloc(n);
}
static void *libc_realloc(void *p, size_t o, size_t n, void *u) {
    (void)o;
    (void)u;
    return realloc(p, n);
}
static void libc_free(void *p, void *u) {
    (void)u;
    free(p);
}

static const flux_allocator libc_allocator = {
    .alloc = libc_alloc,
    .realloc = libc_realloc,
    .free = libc_free,
    .user = nullptr,
};

flux_result flux_arena_init(flux_arena *a, size_t capacity, const flux_allocator *alloc) {
    if (!a || capacity == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    a->alloc = alloc ? *alloc : libc_allocator;
    a->base = a->alloc.alloc(capacity, a->alloc.user);
    if (!a->base)
        return FLUX_ERROR_OUT_OF_MEMORY;
    a->capacity = capacity;
    a->used = 0;
    a->owns_buffer = true;
    return FLUX_OK;
}

void flux_arena_destroy(flux_arena *a) {
    if (!a)
        return;
    if (a->owns_buffer && a->base && a->alloc.free)
        a->alloc.free(a->base, a->alloc.user);
    *a = (flux_arena){0};
}

void *flux_arena_alloc(flux_arena *a, size_t bytes) {
    return flux_arena_alloc_aligned(a, bytes, alignof(max_align_t));
}

void *flux_arena_alloc_aligned(flux_arena *a, size_t bytes, size_t align) {
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

void flux_arena_reset(flux_arena *a) {
    if (a)
        a->used = 0;
}
