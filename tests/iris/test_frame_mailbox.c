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
#include "optics_test.h"

#include <pthread.h>
#include <unistd.h>

static void test_init_destroy_clean(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);
    OT_EXPECT(iris_mailbox_claim(&mb) == nullptr);
    iris_mailbox_destroy(&mb);
}

static void test_sequential_handshake(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    /* UI thread acquires and publishes packet 1 */
    iris_frame_packet *p1 = iris_mailbox_acquire(&mb);
    OT_EXPECT(p1 != nullptr);
    p1->damage[0] = (flux_rect){10, 20, 100, 200};
    p1->damage[1] = (flux_rect){300, 400, 50, 60};
    p1->damage_count = 2;
    p1->scale = 2.0f;
    p1->width = 500;
    p1->height = 500;
    p1->force_paint = true;
    iris_mailbox_publish(&mb, p1);

    /* Compositor claims packet 1 */
    iris_frame_packet *c1 = iris_mailbox_claim(&mb);
    OT_EXPECT(c1 != nullptr);
    OT_EXPECT(c1->sequence == 1);
    OT_EXPECT(c1->scale == 2.0f);
    OT_EXPECT(c1->force_paint == true);
    /* The damage set travels with the snapshot: the presentation thread must
     * never re-query lens. */
    OT_EXPECT(c1->damage_count == 2);
    OT_EXPECT(c1->damage[0].x == 10 && c1->damage[0].y == 20);
    OT_EXPECT(c1->damage[1].x == 300 && c1->damage[1].y == 400);

    /* The damage set survives conversion as a set: two disjoint regions stay two
     * regions, scaled to device pixels and clipped to the surface. This is the
     * property ADR-0143 exists to protect — a collapse here would merge a HUD
     * chip and a dock spring into one near-full-output box. */
    iris_frame_damage set = iris_frame_packet_damage(c1);
    /* p1->force_paint is set, so this packet asks for everything. */
    OT_EXPECT(set.full);
    OT_EXPECT(set.count == 0);

    /* Without the force flag the two rectangles survive individually. The packet
     * declares 500x500 logical at scale 2, so the device extent is 1000x1000 and
     * both rects fit unclipped. */
    c1->force_paint = false;
    set = iris_frame_packet_damage(c1);
    OT_EXPECT(!set.full);
    OT_EXPECT(set.count == 2);
    OT_EXPECT(set.rects[0].x == 20.0f && set.rects[0].y == 40.0f);
    OT_EXPECT(set.rects[0].w == 200.0f && set.rects[0].h == 400.0f);
    OT_EXPECT(set.rects[1].x == 600.0f && set.rects[1].y == 800.0f);
    OT_EXPECT(set.rects[1].w == 100.0f && set.rects[1].h == 120.0f);
    /* The union of the two would be (20,40)-(700,920); the set is strictly
     * smaller than its own bounding box, which is the whole point. */
    OT_EXPECT(set.rects[0].x + set.rects[0].w < set.rects[1].x);

    /* No damage means "paint everything", never "nothing changed". */
    c1->damage_count = 0;
    set = iris_frame_packet_damage(c1);
    OT_EXPECT(set.full);
    OT_EXPECT(set.count == 0);

    /* Second claim before next publish must return NULL */
    OT_EXPECT(iris_mailbox_claim(&mb) == nullptr);

    /* Compositor recycles packet 1 */
    iris_mailbox_recycle(&mb, c1);

    /* UI thread acquires again: should get a valid packet */
    iris_frame_packet *p2 = iris_mailbox_acquire(&mb);
    OT_EXPECT(p2 != nullptr);
    p2->scale = 1.0f;
    iris_mailbox_publish(&mb, p2);

    iris_frame_packet *c2 = iris_mailbox_claim(&mb);
    OT_EXPECT(c2 != nullptr);
    OT_EXPECT(c2->sequence == 2);
    iris_mailbox_recycle(&mb, c2);

    iris_mailbox_destroy(&mb);
}

static void test_fast_producer_supersede(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    /* UI thread publishes three frames in a burst before compositor wakes */
    iris_frame_packet *p1 = iris_mailbox_acquire(&mb);
    OT_EXPECT(p1 != nullptr);
    p1->scale = 1.0f;
    iris_mailbox_publish(&mb, p1);

    iris_frame_packet *p2 = iris_mailbox_acquire(&mb);
    OT_EXPECT(p2 != nullptr);
    p2->scale = 2.0f;
    iris_mailbox_publish(&mb, p2);

    iris_frame_packet *p3 = iris_mailbox_acquire(&mb);
    OT_EXPECT(p3 != nullptr);
    p3->scale = 3.0f;
    iris_mailbox_publish(&mb, p3);

    /* Compositor claims: must get the newest frame (p3) */
    iris_frame_packet *claimed = iris_mailbox_claim(&mb);
    OT_EXPECT(claimed != nullptr);
    OT_EXPECT(claimed->sequence == 3);
    OT_EXPECT(claimed->scale == 3.0f);

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
        p->damage[0] = (flux_rect){0, 0, (float)(i % 100), (float)(i % 100)};
        p->damage_count = 1;
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
            OT_EXPECT(p->sequence > last_seq);
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
    OT_EXPECT(pthread_create(&prod, nullptr, stress_producer_thread, &ctx) == 0);
    OT_EXPECT(pthread_create(&cons, nullptr, stress_consumer_thread, &ctx) == 0);

    pthread_join(prod, nullptr);
    pthread_join(cons, nullptr);

    OT_EXPECT(atomic_load(&ctx.total_published) == 5000);
    OT_EXPECT(atomic_load(&ctx.total_claimed) > 0);

    iris_mailbox_destroy(&ctx.mb);
}

static void test_snapshot_handoff_and_activation_lifecycle(void) {
    iris_frame_mailbox mb;
    iris_mailbox_init(&mb);

    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 240}, .dt_seconds = .016f};
    lens_begin(ui, &in);
    (void)lens_button(ui, &(lens_button_opts){.label = "action",
                                              .box = {.id = "btn", .width = 80, .height = 30}});
    lens_end(ui);

    lens_scene_snapshot *snapshot = nullptr;
    OT_EXPECT(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    OT_EXPECT(snapshot != nullptr);
    OT_EXPECT(lens_last_presented_generation(ui) == 0);

    flux_encoder *enc = nullptr;
    flux_display_list *host_list = nullptr;
    OT_EXPECT(flux_encoder_create(&(flux_encoder_desc){}, &enc) == FLUX_OK);
    flux_encoder_save(enc);
    flux_encoder_clip_rect(enc, (flux_rect){0, 0, 400, 240});
    flux_encoder_restore(enc);
    OT_EXPECT(flux_encoder_finish(enc, &host_list) == FLUX_OK);
    OT_EXPECT(host_list != nullptr);
    flux_encoder_destroy(enc);

    iris_frame_packet *pkt = iris_mailbox_acquire(&mb);
    OT_EXPECT(pkt != nullptr);
    pkt->snapshot = snapshot;
    pkt->host_list = host_list;
    pkt->clear_color = flux_color_rgba(20, 30, 40, 255);
    pkt->has_clear = true;
    pkt->scale = 1.5f;
    pkt->damage[0] = (flux_rect){0, 0, 100, 50};
    pkt->damage_count = 1;
    iris_mailbox_publish(&mb, pkt);

    /* UI thread activates snapshot and releases its local creation handles */
    OT_EXPECT(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    OT_EXPECT(lens_last_presented_generation(ui) == lens_snapshot_generation(snapshot));
    lens_snapshot_release(snapshot);
    flux_display_list_release(host_list);

    /* Presenter thread claims published frame packet */
    iris_frame_packet *claimed = iris_mailbox_claim(&mb);
    OT_EXPECT(claimed != nullptr);
    OT_EXPECT(claimed->snapshot != nullptr);
    OT_EXPECT(claimed->host_list != nullptr);
    OT_EXPECT(claimed->has_clear == true);
    OT_EXPECT(claimed->scale == 1.5f);
    OT_EXPECT(claimed->clear_color == flux_color_rgba(20, 30, 40, 255));

    /* Presenter recycles consumed packet back to channel */
    iris_mailbox_recycle(&mb, claimed);

    /* UI thread acquires next packet: recycled snapshot and host_list are automatically cleaned */
    iris_frame_packet *next_pkt = iris_mailbox_acquire(&mb);
    OT_EXPECT(next_pkt != nullptr);
    OT_EXPECT(next_pkt->snapshot == nullptr);
    OT_EXPECT(next_pkt->host_list == nullptr);

    lens_release(ui);
    iris_mailbox_destroy(&mb);
}

int main(void) {
    test_init_destroy_clean();
    test_sequential_handshake();
    test_fast_producer_supersede();
    test_snapshot_handoff_and_activation_lifecycle();
    test_multithreaded_stress();
    return OT_REPORT();
}
