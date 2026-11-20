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
    optics_window_frame_mailbox mb;
    optics_window_mailbox_init(&mb, nullptr);
    OT_EXPECT(optics_window_mailbox_claim(&mb) == nullptr);
    optics_window_mailbox_destroy(&mb);
}

static void test_sequential_handshake(void) {
    optics_window_frame_mailbox mb;
    optics_window_mailbox_init(&mb, nullptr);

    /* UI thread acquires and publishes packet 1 */
    optics_window_frame_packet *p1 = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(p1 != nullptr);
    p1->damage[0] = (optics_gfx_rect){10, 20, 100, 200};
    p1->damage[1] = (optics_gfx_rect){300, 400, 50, 60};
    p1->damage_count = 2;
    p1->scale = 2.0f;
    p1->width = 500;
    p1->height = 500;
    p1->force_paint = true;
    optics_window_mailbox_publish(&mb, p1);

    /* Compositor claims packet 1 */
    optics_window_frame_packet *c1 = optics_window_mailbox_claim(&mb);
    OT_EXPECT(c1 != nullptr);
    OT_EXPECT(c1->sequence == 1);
    OT_EXPECT(c1->scale == 2.0f);
    OT_EXPECT(c1->force_paint == true);
    /* The damage set travels with the snapshot: the presentation thread must
     * never re-query the UI layer. */
    OT_EXPECT(c1->damage_count == 2);
    OT_EXPECT(c1->damage[0].x == 10 && c1->damage[0].y == 20);
    OT_EXPECT(c1->damage[1].x == 300 && c1->damage[1].y == 400);

    /* The damage set survives conversion as a set: two disjoint regions stay two
     * regions, scaled to device pixels and clipped to the surface. This is the
     * property ADR-0143 exists to protect — a collapse here would merge a HUD
     * chip and a dock spring into one near-full-output box. */
    optics_window_frame_damage set = optics_window_frame_packet_damage(c1);
    /* p1->force_paint is set, so this packet asks for everything. */
    OT_EXPECT(set.full);
    OT_EXPECT(set.count == 0);

    /* Without the force flag the two rectangles survive individually. The packet
     * declares 500x500 logical at scale 2, so the device extent is 1000x1000 and
     * both rects fit unclipped. */
    c1->force_paint = false;
    set = optics_window_frame_packet_damage(c1);
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
    set = optics_window_frame_packet_damage(c1);
    OT_EXPECT(set.full);
    OT_EXPECT(set.count == 0);

    /* Second claim before next publish must return NULL */
    OT_EXPECT(optics_window_mailbox_claim(&mb) == nullptr);

    /* UI thread acquires again: should get a valid packet */
    optics_window_frame_packet *p2 = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(p2 != nullptr);
    p2->scale = 1.0f;
    optics_window_mailbox_publish(&mb, p2);

    optics_window_frame_packet *c2 = optics_window_mailbox_claim(&mb);
    OT_EXPECT(c2 != nullptr);
    OT_EXPECT(c2->sequence == 2);

    optics_window_mailbox_destroy(&mb);
}

static void test_fast_producer_supersede(void) {
    optics_window_frame_mailbox mb;
    optics_window_mailbox_init(&mb, nullptr);

    /* UI thread publishes three frames in a burst before compositor wakes */
    optics_window_frame_packet *p1 = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(p1 != nullptr);
    p1->scale = 1.0f;
    optics_window_mailbox_publish(&mb, p1);

    optics_window_frame_packet *p2 = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(p2 != nullptr);
    p2->scale = 2.0f;
    optics_window_mailbox_publish(&mb, p2);

    optics_window_frame_packet *p3 = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(p3 != nullptr);
    p3->scale = 3.0f;
    optics_window_mailbox_publish(&mb, p3);

    /* Compositor claims: must get the newest frame (p3) */
    optics_window_frame_packet *claimed = optics_window_mailbox_claim(&mb);
    OT_EXPECT(claimed != nullptr);
    OT_EXPECT(claimed->sequence == 3);
    OT_EXPECT(claimed->scale == 3.0f);
    optics_window_mailbox_destroy(&mb);
}

typedef struct stress_context {
    optics_window_frame_mailbox mb;
    _Atomic(bool) done;
    _Atomic(uint64_t) total_published;
    _Atomic(uint64_t) total_claimed;
} stress_context;

static void *stress_producer_thread(void *arg) {
    stress_context *ctx = arg;
    for (uint64_t i = 0; i < 5000; i++) {
        optics_window_frame_packet *p = optics_window_mailbox_acquire(&ctx->mb);
        p->scale = (float)(i + 1);
        p->damage[0] = (optics_gfx_rect){0, 0, (float)(i % 100), (float)(i % 100)};
        p->damage_count = 1;
        optics_window_mailbox_publish(&ctx->mb, p);
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
           optics_window_mailbox_pending(&ctx->mb)) {
        optics_window_frame_packet *p = optics_window_mailbox_claim(&ctx->mb);
        if (p) {
            /* Monotonic sequence ordering verification */
            OT_EXPECT(p->sequence > last_seq);
            last_seq = p->sequence;
            atomic_fetch_add_explicit(&ctx->total_claimed, 1, memory_order_relaxed);
        } else {
            usleep(20);
        }
    }
    return nullptr;
}

static void test_multithreaded_stress(void) {
    stress_context ctx;
    optics_window_mailbox_init(&ctx.mb, nullptr);
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

    optics_window_mailbox_destroy(&ctx.mb);
}

/* A stand-in UI layer: one refcounted snapshot type and nothing else. The
 * mailbox must retain and release packet snapshots through the host table —
 * it has no other way to know what a snapshot is. */
struct optics_window_ui_snapshot {
    int refs;
};

static optics_window_ui_snapshot *fake_retain(optics_window_ui_snapshot *s) {
    s->refs++;
    return s;
}
static void fake_release(optics_window_ui_snapshot *s) {
    s->refs--;
}

static void test_snapshot_handoff_lifecycle(void) {
    const optics_window_ui_host host = {.snapshot_retain = fake_retain,
                                        .snapshot_release = fake_release};
    optics_window_frame_mailbox mb;
    optics_window_mailbox_init(&mb, &host);

    optics_window_ui_snapshot snap = {.refs = 1}; /* the UI thread's creation reference */

    optics_gfx_encoder *enc = nullptr;
    optics_gfx_display_list *host_list = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(&(optics_gfx_encoder_desc){}, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_save(enc);
    optics_gfx_encoder_clip_rect(enc, (optics_gfx_rect){0, 0, 400, 240});
    optics_gfx_encoder_restore(enc);
    OT_EXPECT(optics_gfx_encoder_finish(enc, &host_list) == OPTICS_GFX_OK);
    OT_EXPECT(host_list != nullptr);
    optics_gfx_encoder_destroy(enc);

    optics_window_frame_packet *pkt = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(pkt != nullptr);
    pkt->snapshot = &snap;
    pkt->host_list = host_list;
    pkt->clear_color = optics_gfx_color_rgba(20, 30, 40, 255);
    pkt->has_clear = true;
    pkt->scale = 1.5f;
    pkt->damage[0] = (optics_gfx_rect){0, 0, 100, 50};
    pkt->damage_count = 1;
    optics_window_mailbox_publish(&mb, pkt);
    /* Publishing takes the mailbox's own reference. */
    OT_EXPECT(snap.refs == 2);

    /* UI thread drops its creation handles; the packet keeps both alive. */
    fake_release(&snap);
    optics_gfx_display_list_release(host_list);
    OT_EXPECT(snap.refs == 1);

    /* Presenter thread claims published frame packet */
    optics_window_frame_packet *claimed = optics_window_mailbox_claim(&mb);
    OT_EXPECT(claimed != nullptr);
    OT_EXPECT(claimed->snapshot == &snap);
    OT_EXPECT(claimed->host_list != nullptr);
    OT_EXPECT(claimed->has_clear == true);
    OT_EXPECT(claimed->scale == 1.5f);
    OT_EXPECT(claimed->clear_color == optics_gfx_color_rgba(20, 30, 40, 255));

    /* The presented packet keeps its references until it travels back to the
     * UI thread: two publishes later it is the UI's back buffer again, and
     * acquire releases what it held — on the UI thread, exactly once. */
    optics_window_frame_packet *next_pkt = nullptr;
    for (int i = 0; i < 2; i++) {
        next_pkt = optics_window_mailbox_acquire(&mb);
        OT_EXPECT(next_pkt != nullptr);
        optics_window_mailbox_publish(&mb, next_pkt);
        OT_EXPECT(optics_window_mailbox_claim(&mb) == next_pkt);
    }
    OT_EXPECT(snap.refs == 1);
    next_pkt = optics_window_mailbox_acquire(&mb);
    OT_EXPECT(next_pkt != nullptr);
    OT_EXPECT(next_pkt->snapshot == nullptr);
    OT_EXPECT(next_pkt->host_list == nullptr);
    OT_EXPECT(snap.refs == 0);

    /* A snapshot still parked in the mailbox at destroy is released too. */
    optics_window_ui_snapshot parked = {.refs = 1};
    next_pkt->snapshot = &parked;
    optics_window_mailbox_publish(&mb, next_pkt);
    fake_release(&parked);
    OT_EXPECT(parked.refs == 1);
    optics_window_mailbox_destroy(&mb);
    OT_EXPECT(parked.refs == 0);
}

/* Regression: the previous design had a fourth "recycle" slot, and returning a
 * packet while that slot was occupied dropped a packet from circulation — the
 * buffer degraded to two packets and the UI thread could spin waiting for one.
 * A triple buffer has no such state: after any interleaving the three packets
 * are still three distinct packets, one per role. */
static void test_no_packet_is_ever_lost(void) {
    optics_window_frame_mailbox mb;
    optics_window_mailbox_init(&mb, nullptr);
    uint32_t state = 12345u;
    for (int step = 0; step < 100000; step++) {
        state = state * 1664525u + 1013904223u;
        if ((state >> 16) & 1) {
            optics_window_frame_packet *p = optics_window_mailbox_acquire(&mb);
            OT_REQUIRE(p != nullptr);
            optics_window_mailbox_publish(&mb, p);
        } else {
            (void)optics_window_mailbox_claim(&mb);
        }
        const uint32_t mid = atomic_load(&mb.middle) & 0x3u;
        OT_REQUIRE(mb.back < 3 && mb.front < 3 && mid < 3);
        OT_REQUIRE(mb.back != mb.front && mb.back != mid && mb.front != mid);
    }
    optics_window_mailbox_destroy(&mb);
}

int main(void) {
    test_init_destroy_clean();
    test_sequential_handshake();
    test_fast_producer_supersede();
    test_snapshot_handoff_lifecycle();
    test_no_packet_is_ever_lost();
    test_multithreaded_stress();
    return OT_REPORT();
}
