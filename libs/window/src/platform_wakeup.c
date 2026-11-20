/* platform_wakeup.c — shared queue behind the backend wakeup seam.
 *
 * See platform_wakeup.h for the contract. This file is deliberately
 * tiny: a lock-protected FIFO of (fn, user) pairs plus the registered
 * kick. The lock is a portable shim in the flux_platform_mutex style
 * (libs/flux/src/core/platform.h): SRWLOCK on Windows, pthread_mutex
 * elsewhere — only the three-function contract in the header matters
 * to the rest of iris.
 */

#include "platform_wakeup.h"

#include <stdbool.h>
#include <stdlib.h>

#if defined(_WIN32)
/* Keep the windows.h surface minimal, mirroring flux's platform.h. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef SRWLOCK optics_window_wakeup_mutex;
#define OPTICS_WINDOW_WAKEUP_MUTEX_INIT SRWLOCK_INIT
static inline void optics_window_wakeup_lock(optics_window_wakeup_mutex *m) {
    AcquireSRWLockExclusive(m);
}
static inline void optics_window_wakeup_unlock(optics_window_wakeup_mutex *m) {
    ReleaseSRWLockExclusive(m);
}
#else
#include <pthread.h>

typedef pthread_mutex_t optics_window_wakeup_mutex;
#define OPTICS_WINDOW_WAKEUP_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void optics_window_wakeup_lock(optics_window_wakeup_mutex *m) {
    pthread_mutex_lock(m);
}
static inline void optics_window_wakeup_unlock(optics_window_wakeup_mutex *m) {
    pthread_mutex_unlock(m);
}
#endif

typedef struct optics_window_wakeup_job {
    optics_window_wakeup_fn fn;
    void *user;
    struct optics_window_wakeup_job *next;
} optics_window_wakeup_job;

static optics_window_wakeup_mutex g_lock = OPTICS_WINDOW_WAKEUP_MUTEX_INIT;
static optics_window_wakeup_kick_fn g_kick = nullptr;
static void *g_kick_user = nullptr;
static optics_window_wakeup_job *g_head = nullptr;
static optics_window_wakeup_job *g_tail = nullptr;

void optics_window_platform_wakeup_set_kick(optics_window_wakeup_kick_fn kick, void *user) {
    optics_window_wakeup_lock(&g_lock);
    g_kick = kick;
    g_kick_user = user;
    optics_window_wakeup_unlock(&g_lock);
}

int optics_window_platform_wakeup_post(optics_window_wakeup_fn fn, void *user) {
    if (!fn)
        return -1;

    optics_window_wakeup_job *job = malloc(sizeof *job);
    if (!job)
        return -1;
    job->fn = fn;
    job->user = user;
    job->next = nullptr;

    optics_window_wakeup_kick_fn kick;
    void *kick_user;
    optics_window_wakeup_lock(&g_lock);
    if (!g_kick) {
        /* No loop registered: drop the callback (documented semantics). */
        optics_window_wakeup_unlock(&g_lock);
        free(job);
        return -1;
    }
    if (g_tail)
        g_tail->next = job;
    else
        g_head = job;
    g_tail = job;
    kick = g_kick;
    kick_user = g_kick_user;
    optics_window_wakeup_unlock(&g_lock);

    /* Kick outside the lock: the kick must not block, and this ordering
     * guarantees the job is visible to any drain that the kick provokes. */
    kick(kick_user);
    return 0;
}

void optics_window_platform_wakeup_drain(void) {
    for (;;) {
        optics_window_wakeup_lock(&g_lock);
        optics_window_wakeup_job *job = g_head;
        if (job) {
            g_head = job->next;
            if (!g_head)
                g_tail = nullptr;
        }
        optics_window_wakeup_unlock(&g_lock);

        if (!job)
            return;
        /* Run outside the lock so a callback may re-post without
         * deadlocking; such a job is picked up by the next iteration. */
        job->fn(job->user);
        free(job);
    }
}
