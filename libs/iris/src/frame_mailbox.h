/* frame_mailbox.h — wait-free triple-buffered snapshot exchange (ADR-0124).
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
#include <iris/host.h>

typedef struct iris_frame_packet {
    iris_ui_snapshot *snapshot;   /* retained reference */
    flux_display_list *host_list; /* optional retained host recorded content (ADR-0127) */
    uint64_t sequence;
    /* Damage as a set, not a box: the UI thread resolves the frame's regions and
     * the presentation thread consumes them without a second query (ADR-0143).
     * `damage_count == 0` means "everything" — a packet that could not describe
     * its damage must never be mistaken for "nothing changed". */
    flux_rect damage[IRIS_DAMAGE_RECTS_CAPACITY];
    uint32_t damage_count;
    bool force_paint;
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
    flux_rect rects[IRIS_DAMAGE_RECTS_CAPACITY];
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
[[nodiscard]] iris_frame_damage iris_frame_ui_damage(const iris_ui_host *ui, float scale,
                                                     int32_t width, int32_t height,
                                                     bool force_full);

/* A classic triple buffer. Three packets, three roles, and one atomic word:
 *
 *   back    owned by the UI thread — the packet it is filling.
 *   middle  the shared slot — the newest published packet, or a spare.
 *   front   owned by the presentation thread — the packet it is drawing.
 *
 * Publishing swaps `back` with `middle`; claiming swaps `front` with `middle`.
 * Each side always owns exactly one packet and the third is always in the
 * shared slot, so no packet can be lost, neither side can ever wait for the
 * other, and a burst of publishes simply overwrites the middle (the newest
 * frame wins). There is no fourth state to get wrong. */
typedef struct iris_frame_mailbox {
    iris_frame_packet pool[3];
    _Atomic(uint32_t) middle; /* pool index, OR'd with IRIS_MAILBOX_FRESH when unclaimed */
    uint32_t back;            /* UI thread only           */
    uint32_t front;           /* presentation thread only */
    _Atomic(uint64_t) sequence;
    const iris_ui_host *ui; /* releases packet snapshots; may be null */
} iris_frame_mailbox;

/* `ui` owns the snapshots travelling through the mailbox; it releases them. */
void iris_mailbox_init(iris_frame_mailbox *mb, const iris_ui_host *ui);

/* Release every reference still parked in the mailbox. Call after the
 * presentation thread has stopped. */
void iris_mailbox_destroy(iris_frame_mailbox *mb);

/* UI thread: the packet to fill for the next frame. Never blocks, never
 * fails. References left in it by a superseded or already-presented frame are
 * released here, on the UI thread. */
[[nodiscard]] iris_frame_packet *iris_mailbox_acquire(iris_frame_mailbox *mb);

/* UI thread: publish the packet returned by the last acquire. Takes its own
 * references on the packet's snapshot and host list. Never blocks. */
void iris_mailbox_publish(iris_frame_mailbox *mb, iris_frame_packet *pkt);

/* Presentation thread: the newest published packet, or nullptr when nothing
 * new was published since the last claim. The packet stays valid until the
 * next claim. Never blocks. */
[[nodiscard]] iris_frame_packet *iris_mailbox_claim(iris_frame_mailbox *mb);

/* Any thread: true while a published packet is waiting to be claimed. */
[[nodiscard]] bool iris_mailbox_pending(const iris_frame_mailbox *mb);

#endif /* IRIS_FRAME_MAILBOX_H */
