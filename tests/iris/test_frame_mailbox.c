/* test_frame_mailbox.c — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Verifies the wait-free atomic handover channel between the UI event thread
 * (Thread 0) and the Compositor presentation thread (Thread 1):
 *   - INV-THREAD-01: Non-blocking UI event loop (acquire/publish never block).
 *   - INV-THREAD-02: Lockless immutable snapshot exchange (atomic pointer exchange).
 *   - Monotonic sequence ordering and superseding under producer burst.
 *   - Multithreaded concurrent producer/consumer stress test.
 */

#include "frame_mailbox.h"
#include "test_helpers.h"

#include <pthread.h>
#include <unistd.h>

static void test_init_destroy_clean(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);
    CHECK(iris_mailbox_claim(&mb) == nullptr);
    iris_mailbox_destroy(&mb);
}

static void test_sequential_handshake(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    /* UI thread acquires and publishes packet 1 */
    iris_frame_packet *p1 = iris_mailbox_acquire(&mb);
    CHECK(p1 != nullptr);
    p1->damage_rect = (flux_rect){10, 20, 100, 200};
    p1->scale = 2.0f;
    p1->force_paint = true;
    iris_mailbox_publish(&mb, p1);

    /* Compositor claims packet 1 */
    iris_frame_packet *c1 = iris_mailbox_claim(&mb);
    CHECK(c1 != nullptr);
    CHECK(c1->sequence == 1);
    CHECK(c1->scale == 2.0f);
    CHECK(c1->force_paint == true);
    CHECK(c1->damage_rect.x == 10 && c1->damage_rect.y == 20);

    /* Second claim before next publish must return NULL */
    CHECK(iris_mailbox_claim(&mb) == nullptr);

    /* Compositor recycles packet 1 */
    iris_mailbox_recycle(&mb, c1);

    /* UI thread acquires again: should get a valid packet */
    iris_frame_packet *p2 = iris_mailbox_acquire(&mb);
    CHECK(p2 != nullptr);
    p2->scale = 1.0f;
    iris_mailbox_publish(&mb, p2);

    iris_frame_packet *c2 = iris_mailbox_claim(&mb);
    CHECK(c2 != nullptr);
    CHECK(c2->sequence == 2);
    iris_mailbox_recycle(&mb, c2);

    iris_mailbox_destroy(&mb);
}

static void test_fast_producer_supersede(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    /* UI thread publishes three frames in a burst before compositor wakes */
    iris_frame_packet *p1 = iris_mailbox_acquire(&mb);
    CHECK(p1 != nullptr);
    p1->scale = 1.0f;
    iris_mailbox_publish(&mb, p1);

    iris_frame_packet *p2 = iris_mailbox_acquire(&mb);
    CHECK(p2 != nullptr);
    p2->scale = 2.0f;
    iris_mailbox_publish(&mb, p2);

    iris_frame_packet *p3 = iris_mailbox_acquire(&mb);
    CHECK(p3 != nullptr);
    p3->scale = 3.0f;
    iris_mailbox_publish(&mb, p3);

    /* Compositor claims: must get the newest frame (p3) */
    iris_frame_packet *claimed = iris_mailbox_claim(&mb);
    CHECK(claimed != nullptr);
    CHECK(claimed->sequence == 3);
    CHECK(claimed->scale == 3.0f);

    iris_mailbox_recycle(&mb, claimed);
    iris_mailbox_destroy(&mb);
}

typedef struct stress_context {
    iris_frame_mailbox mb;
    _Atomic(bool) done;
    _Atomic(uint64_t) total_published;
    _Atomic(uint64_t) total_claimed;
} stress_context;

static void *stress_producer_thread(void *arg) {
    stress_context *ctx = arg;
    for (uint64_t i = 0; i < 5000; i++) {
        iris_frame_packet *p = iris_mailbox_acquire(&ctx->mb);
        p->scale = (float)(i + 1);
        p->damage_rect = (flux_rect){0, 0, (float)(i % 100), (float)(i % 100)};
        iris_mailbox_publish(&ctx->mb, p);
        atomic_fetch_add_explicit(&ctx->total_published, 1, memory_order_relaxed);
        if ((i % 64) == 0)
            usleep(10);
    }
    atomic_store_explicit(&ctx->done, true, memory_order_release);
    return nullptr;
}

static void *stress_consumer_thread(void *arg) {
    stress_context *ctx = arg;
    uint64_t last_seq = 0;
    while (!atomic_load_explicit(&ctx->done, memory_order_acquire) ||
           atomic_load_explicit(&ctx->mb.ready, memory_order_relaxed) != nullptr) {
        iris_frame_packet *p = iris_mailbox_claim(&ctx->mb);
        if (p) {
            /* Monotonic sequence ordering verification */
            CHECK(p->sequence > last_seq);
            last_seq = p->sequence;
            atomic_fetch_add_explicit(&ctx->total_claimed, 1, memory_order_relaxed);
            iris_mailbox_recycle(&ctx->mb, p);
        } else {
            usleep(20);
        }
    }
    return nullptr;
}

static void test_multithreaded_stress(void) {
    stress_context ctx;
    iris_mailbox_init(&ctx.mb);
    atomic_init(&ctx.done, false);
    atomic_init(&ctx.total_published, 0);
    atomic_init(&ctx.total_claimed, 0);

    pthread_t prod, cons;
    CHECK(pthread_create(&prod, nullptr, stress_producer_thread, &ctx) == 0);
    CHECK(pthread_create(&cons, nullptr, stress_consumer_thread, &ctx) == 0);

    pthread_join(prod, nullptr);
    pthread_join(cons, nullptr);

    CHECK(atomic_load(&ctx.total_published) == 5000);
    CHECK(atomic_load(&ctx.total_claimed) > 0);

    iris_mailbox_destroy(&ctx.mb);
}

static void test_snapshot_handoff_and_activation_lifecycle(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    lens *ui = nullptr;
    CHECK(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 240}, .dt_seconds = .016f};
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "action",
                                              .box = {.id = "btn", .width = 80, .height = 30}});
    lens_end(ui);

    lens_scene_snapshot *snapshot = nullptr;
    CHECK(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    CHECK(snapshot != nullptr);
    CHECK(lens_last_presented_generation(ui) == 0);

    flux_encoder *enc = nullptr;
    flux_display_list *host_list = nullptr;
    CHECK(flux_encoder_create(&(flux_encoder_desc){}, &enc) == FLUX_OK);
    flux_encoder_save(enc);
    flux_encoder_clip_rect(enc, (flux_rect){0, 0, 400, 240});
    flux_encoder_restore(enc);
    CHECK(flux_encoder_finish(enc, &host_list) == FLUX_OK);
    CHECK(host_list != nullptr);
    flux_encoder_destroy(enc);

    iris_frame_packet *pkt = iris_mailbox_acquire(&mb);
    CHECK(pkt != nullptr);
    pkt->snapshot = snapshot;
    pkt->host_list = host_list;
    pkt->clear_color = flux_color_rgba(20, 30, 40, 255);
    pkt->has_clear = true;
    pkt->scale = 1.5f;
    pkt->damage_rect = (flux_rect){0, 0, 100, 50};
    iris_mailbox_publish(&mb, pkt);

    /* UI thread activates snapshot and releases its local creation handles */
    CHECK(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    CHECK(lens_last_presented_generation(ui) == lens_snapshot_generation(snapshot));
    lens_snapshot_release(snapshot);
    flux_display_list_release(host_list);

    /* Presenter thread claims published frame packet */
    iris_frame_packet *claimed = iris_mailbox_claim(&mb);
    CHECK(claimed != nullptr);
    CHECK(claimed->snapshot != nullptr);
    CHECK(claimed->host_list != nullptr);
    CHECK(claimed->has_clear == true);
    CHECK(claimed->scale == 1.5f);
    CHECK(claimed->clear_color == flux_color_rgba(20, 30, 40, 255));

    /* Presenter recycles consumed packet back to channel */
    iris_mailbox_recycle(&mb, claimed);

    /* UI thread acquires next packet: recycled snapshot and host_list are automatically cleaned */
    iris_frame_packet *next_pkt = iris_mailbox_acquire(&mb);
    CHECK(next_pkt != nullptr);
    CHECK(next_pkt->snapshot == nullptr);
    CHECK(next_pkt->host_list == nullptr);

    lens_release(ui);
    iris_mailbox_destroy(&mb);
}

int main(void) {
    test_init_destroy_clean();
    test_sequential_handshake();
    test_fast_producer_supersede();
    test_snapshot_handoff_and_activation_lifecycle();
    test_multithreaded_stress();
    return TEST_REPORT();
}
