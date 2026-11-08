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
    flux_display_list *host_list;  /* optional retained host recorded content (ADR-0127) */
    uint64_t sequence;
    /* Damage as a set, not a box: the UI thread resolves the frame's regions and
     * the presentation thread consumes them without a second query (ADR-0143).
     * `damage_count == 0` means "everything" — a packet that could not describe
     * its damage must never be mistaken for "nothing changed". */
    flux_rect damage[LENS_DAMAGE_RECTS_CAPACITY];
    uint32_t damage_count;
    bool force_paint;
    bool has_custom_paint;
    bool has_clear;
    flux_color clear_color;
    float scale;
    int32_t width;
    int32_t height;
} iris_frame_packet;

/* Device-pixel damage scope for one frame, ready to hand to
 * `flux_canvas_pass_desc.damage` (ADR-0149).
 *
 * The conversion from UI-logical to device pixels happens here, once, because
 * it is the boundary at which the producer's units stop mattering: a backend that
 * scissors each rectangle independently, one that hands the set to a per-plane
 * damage hint, and one whose API takes a single rectangle all start from the same
 * array. Collapsing to one rectangle before this point destroys the localisation
 * irrecoverably (ADR-0143), so no helper here does it.
 *
 * `force` means "the whole frame is new" (a resize, a scale change, the very
 * first frame): the resulting scope is empty *and* flagged, which the canvas
 * reads as the full destination. An empty scope with `force` false would be
 * indistinguishable from "nothing changed", which is why the flag travels
 * separately rather than being encoded as a zero rectangle. */
typedef struct iris_frame_damage {
    flux_rect rects[LENS_DAMAGE_RECTS_CAPACITY];
    uint32_t count;
    bool full; /* every pixel may have changed */
} iris_frame_damage;

/* Resolve the packet's damage set into device pixels. A packet whose
 * `force_paint` is set, whose set is empty, or whose regions all fall outside the
 * surface, yields `full = true` — never "nothing". The device extent is derived
 * from the packet's logical size and scale, so the caller never has to know which
 * unit the clip uses. */
[[nodiscard]] iris_frame_damage iris_frame_packet_damage(const iris_frame_packet *pkt);

/* The same resolution for a host that presents lens directly, without the
 * mailbox (the single-threaded path). `width`/`height` are the surface's *device*
 * pixel extent (as reported by `flux_surface_get_info`), because this path has no
 * packet to derive them from. `force_full` is the host's own "everything
 * changed" signal. */
[[nodiscard]] iris_frame_damage iris_frame_lens_damage(const lens *ui, float scale, int32_t width,
                                                       int32_t height, bool force_full);

typedef struct iris_frame_mailbox {
    _Atomic(iris_frame_packet *) ready;   /* published by UI, claimed by compositor */
    _Atomic(iris_frame_packet *) recycle; /* returned by compositor, reused by UI   */
    iris_frame_packet pool[3];            /* triple buffer backing store            */
    iris_frame_packet *spare;             /* UI thread's local spare buffer         */
    iris_frame_packet *spare2;            /* UI thread's secondary spare buffer     */
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
