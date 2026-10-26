/* frame_mailbox.h — lockless triple-buffered snapshot exchange (ADR-0124).
 *
 * Establishes the thread-safe handoff channel between the UI / Event thread
 * (Thread 0) and the Compositor / Presentation thread (Thread 1).
 *
 * Design constraints:
 *   - Pure: lockless atomic pointer exchange (stdatomic.h). No mutexes on the
 *     hot path, preventing priority inversion between 1000Hz input and
 *     display VSync (INV-THREAD-01, INV-THREAD-02).
 *   - Triple-buffered: UI thread can publish at any rate; compositor thread
 *     always gets the latest complete snapshot without blocking.
 *   - Reference-counted: snapshots are retained on publish and released on
 *     recycling or mailbox destruction.
 */
#ifndef IRIS_FRAME_MAILBOX_H
#define IRIS_FRAME_MAILBOX_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <flux/math.h>
#include <lens/lens.h>

typedef struct iris_frame_packet {
    lens_scene_snapshot *snapshot; /* retained reference */
    uint64_t sequence;
    flux_rect damage_rect;
    bool force_paint;
    bool has_custom_paint;
    float scale;
    int32_t width;
    int32_t height;
} iris_frame_packet;

typedef struct iris_frame_mailbox {
    _Atomic(iris_frame_packet *) ready;   /* published by UI, claimed by compositor */
    _Atomic(iris_frame_packet *) recycle; /* returned by compositor, reused by UI   */
    iris_frame_packet pool[3];            /* triple buffer backing store            */
    iris_frame_packet *spare;             /* UI thread's local spare buffer         */
    _Atomic(uint64_t) sequence;
} iris_frame_mailbox;

void iris_mailbox_init(iris_frame_mailbox *mb);
void iris_mailbox_destroy(iris_frame_mailbox *mb);

/* UI Thread: acquire an idle packet from the pool or recycle bin. Never blocks. */
[[nodiscard]] iris_frame_packet *iris_mailbox_acquire(iris_frame_mailbox *mb);

/* UI Thread: publish a populated packet. Retains snapshot. Never blocks. */
void iris_mailbox_publish(iris_frame_mailbox *mb, iris_frame_packet *pkt);

/* Compositor Thread: claim the newest published packet, or returns NULL if no
 * new frame was published since the last claim. Never blocks. */
[[nodiscard]] iris_frame_packet *iris_mailbox_claim(iris_frame_mailbox *mb);

/* Compositor Thread: return a consumed packet back to the recycle queue. */
void iris_mailbox_recycle(iris_frame_mailbox *mb, iris_frame_packet *pkt);

#endif /* IRIS_FRAME_MAILBOX_H */
