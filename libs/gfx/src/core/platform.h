/*
 * OS shims for gfx. Never installed.
 *
 * The one OS facility gfx needs that C23 does not provide: a non-recursive
 * mutex (SRWLOCK on Windows, pthread_mutex elsewhere). Overflow-checked
 * arithmetic is C23 <stdckdint.h>; strdup is C23 <string.h>.
 */
#ifndef OPTICS_GFX_CORE_PLATFORM_H
#define OPTICS_GFX_CORE_PLATFORM_H

#include <stdbool.h>

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
/*  Non-recursive exclusive lock. An SRWLOCK owns no kernel resources, */
/*  so destroy is a no-op on Windows.                                  */
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

#endif /* OPTICS_GFX_CORE_PLATFORM_H */
