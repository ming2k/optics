/* test_platform_wakeup.c — cross-thread main-loop wakeup queue contract.
 *
 * Verifies the shared FIFO queue behind optics_window_post_to_main_thread:
 *   - null callback and unregistered-kick posts fail with -1 and never run;
 *   - single-threaded FIFO ordering across bursts;
 *   - re-entrant post from inside a draining callback never deadlocks and runs;
 *   - unregistering the kick immediately rejects subsequent posts;
 *   - concurrent multi-producer stress test delivers every job exactly once.
 */

#include "optics_test.h"
#include "platform_wakeup.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

static atomic_int g_kicks = 0;

static void count_kick(void *user) {
    atomic_int *counter = user;
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

typedef struct fifo_ctx {
    int order[64];
    int count;
} fifo_ctx;

typedef struct fifo_item {
    fifo_ctx *ctx;
    int value;
} fifo_item;

static void record_fifo(void *user) {
    fifo_item *item = user;
    if (item->ctx->count < 64)
        item->ctx->order[item->ctx->count++] = item->value;
}

typedef struct reentrant_ctx {
    int depth;
    int target;
    int order[8];
} reentrant_ctx;

static void reentrant_job(void *user) {
    reentrant_ctx *rc = user;
    int cur = rc->depth;
    if (cur < 8)
        rc->order[cur] = cur;
    rc->depth++;
    if (rc->depth < rc->target) {
        OT_EXPECT(optics_window_platform_wakeup_post(reentrant_job, rc) == 0);
    }
}

static void noop_job(void *user) {
    int *ran = user;
    if (ran)
        (*ran)++;
}

static void test_unregistered_and_null(void) {
    optics_window_platform_wakeup_set_kick(nullptr, nullptr);
    int ran = 0;
    OT_EXPECT(optics_window_platform_wakeup_post(nullptr, nullptr) == -1);
    OT_EXPECT(optics_window_platform_wakeup_post(noop_job, &ran) == -1);
    optics_window_platform_wakeup_drain();
    OT_EXPECT(ran == 0);

    atomic_store(&g_kicks, 0);
    optics_window_platform_wakeup_set_kick(count_kick, &g_kicks);
    OT_EXPECT(optics_window_platform_wakeup_post(nullptr, nullptr) == -1);
    OT_EXPECT(atomic_load(&g_kicks) == 0);
    optics_window_platform_wakeup_set_kick(nullptr, nullptr);
}

static void test_fifo_and_reentrant(void) {
    atomic_store(&g_kicks, 0);
    optics_window_platform_wakeup_set_kick(count_kick, &g_kicks);

    fifo_ctx ctx = {};
    fifo_item items[32];
    for (int i = 0; i < 32; ++i) {
        items[i] = (fifo_item){.ctx = &ctx, .value = i + 100};
        OT_EXPECT(optics_window_platform_wakeup_post(record_fifo, &items[i]) == 0);
    }
    OT_EXPECT(atomic_load(&g_kicks) == 32);

    optics_window_platform_wakeup_drain();
    OT_EXPECT(ctx.count == 32);
    for (int i = 0; i < 32; ++i)
        OT_EXPECT(ctx.order[i] == i + 100);

    /* Draining an empty queue is a safe no-op. */
    optics_window_platform_wakeup_drain();
    OT_EXPECT(ctx.count == 32);

    /* Re-entrant post from inside a draining callback must not deadlock and
     * must be picked up by the drain loop. */
    reentrant_ctx rc = {.depth = 0, .target = 6};
    OT_EXPECT(optics_window_platform_wakeup_post(reentrant_job, &rc) == 0);
    optics_window_platform_wakeup_drain();
    OT_EXPECT(rc.depth == 6);
    for (int i = 0; i < 6; ++i)
        OT_EXPECT(rc.order[i] == i);

    /* Unregistering the kick rejects new posts immediately. */
    optics_window_platform_wakeup_set_kick(nullptr, nullptr);
    int after = 0;
    OT_EXPECT(optics_window_platform_wakeup_post(noop_job, &after) == -1);
    optics_window_platform_wakeup_drain();
    OT_EXPECT(after == 0);
}

#define PRODUCER_THREADS 4
#define JOBS_PER_THREAD 500

typedef struct stress_state {
    atomic_int executed;
    atomic_int kick_count;
    atomic_int producers_done;
} stress_state;

static void stress_job(void *user) {
    stress_state *st = user;
    atomic_fetch_add_explicit(&st->executed, 1, memory_order_relaxed);
}

static void *producer_main(void *arg) {
    stress_state *st = arg;
    for (int i = 0; i < JOBS_PER_THREAD; ++i) {
        int rc = optics_window_platform_wakeup_post(stress_job, st);
        OT_EXPECT(rc == 0);
    }
    atomic_fetch_add_explicit(&st->producers_done, 1, memory_order_release);
    return nullptr;
}

static void test_multithreaded_stress(void) {
    stress_state st = {};
    optics_window_platform_wakeup_set_kick(count_kick, &st.kick_count);

    pthread_t threads[PRODUCER_THREADS];
    for (int i = 0; i < PRODUCER_THREADS; ++i)
        OT_REQUIRE(pthread_create(&threads[i], nullptr, producer_main, &st) == 0);

    while (atomic_load_explicit(&st.producers_done, memory_order_acquire) < PRODUCER_THREADS)
        optics_window_platform_wakeup_drain();

    for (int i = 0; i < PRODUCER_THREADS; ++i)
        OT_EXPECT(pthread_join(threads[i], nullptr) == 0);

    /* Final drain guarantees every job posted before join has executed. */
    optics_window_platform_wakeup_drain();

    OT_EXPECT(atomic_load(&st.executed) == PRODUCER_THREADS * JOBS_PER_THREAD);
    OT_EXPECT(atomic_load(&st.kick_count) == PRODUCER_THREADS * JOBS_PER_THREAD);

    optics_window_platform_wakeup_set_kick(nullptr, nullptr);
}

int main(void) {
    test_unregistered_and_null();
    test_fifo_and_reentrant();
    test_multithreaded_stress();
    return OT_REPORT();
}
