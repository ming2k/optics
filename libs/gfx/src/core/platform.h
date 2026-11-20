/*
 * Platform abstraction shims. Never installed.
 *
 * flux targets Linux, Windows (MSVC / clang-cl / MinGW) and macOS. This
 * header funnels the handful of OS facilities the library needs through
 * one place so the rest of the tree stays platform-neutral:
 *   - flux_platform_mutex: SRWLOCK on Windows, pthread_mutex elsewhere.
 *   - flux_platform_mul_size: overflow-checked size_t multiplication.
 *   - flux_platform_strdup: strdup without the POSIX name (MSVC spells
 *     it _strdup and deprecates the plain one).
 */
#ifndef OPTICS_GFX_CORE_PLATFORM_H
#define OPTICS_GFX_CORE_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Keep the windows.h surface minimal: no min/max macros, no GDI/user
 * grab-bag. SRWLOCK lives behind both guards on every Windows toolchain. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ */
/*  Mutex                                                             */
/*                                                                    */
/*  Non-recursive exclusive lock. Call sites keep the existing field   */
/*  pattern `flux_platform_mutex lock; bool lock_initialized;`: init   */
/*  sets the flag, destroy consults it. An SRWLOCK owns no kernel      */
/*  resources, so destroy is intentionally a no-op on Windows.         */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
typedef SRWLOCK optics_gfx_platform_mutex;
#else
typedef pthread_mutex_t optics_gfx_platform_mutex;
#endif

static inline bool optics_gfx_platform_mutex_init(optics_gfx_platform_mutex *m) {
#if defined(_WIN32)
    InitializeSRWLock(m);
    return true;
#else
    return pthread_mutex_init(m, nullptr) == 0;
#endif
}

static inline void optics_gfx_platform_mutex_destroy(optics_gfx_platform_mutex *m) {
#if defined(_WIN32)
    (void)m;
#else
    pthread_mutex_destroy(m);
#endif
}

static inline void optics_gfx_platform_mutex_lock(optics_gfx_platform_mutex *m) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(m);
#else
    pthread_mutex_lock(m);
#endif
}

static inline void optics_gfx_platform_mutex_unlock(optics_gfx_platform_mutex *m) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive(m);
#else
    pthread_mutex_unlock(m);
#endif
}

/* ------------------------------------------------------------------ */
/*  Overflow-checked size multiplication                              */
/* ------------------------------------------------------------------ */

#ifdef __has_builtin
#if __has_builtin(__builtin_mul_overflow)
#define OPTICS_GFX_PLATFORM_HAVE_BUILTIN_MUL_OVERFLOW 1
#endif
#endif

/* *out = a * b, reporting false instead of wrapping on overflow. */
static inline bool optics_gfx_platform_mul_size(size_t a, size_t b, size_t *out) {
#if defined(OPTICS_GFX_PLATFORM_HAVE_BUILTIN_MUL_OVERFLOW)
    return !__builtin_mul_overflow(a, b, out);
#else
    /* Portable form: MSVC has no __builtin_mul_overflow. */
    if (b != 0 && a > SIZE_MAX / b)
        return false;
    *out = a * b;
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/*  String duplication                                                */
/* ------------------------------------------------------------------ */

/* malloc + memcpy; the POSIX strdup name is deprecated on MSVC (it wants
 * _strdup), so spell the two operations out. Caller frees with free(). */
static inline char *optics_gfx_platform_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

#endif /* FLUX_CORE_PLATFORM_H */
