/*
 * damage_history.h — the write-epoch history a swapchain rotation is repaired
 * from (ADR-0149).
 *
 * A windowed surface renders into one persistent content image. The acquired
 * swapchain image is a *presentation* buffer, and which one it is changes every
 * frame. For the images to agree, each acquired image must receive exactly the
 * pixels written since the last time *that* image was presented.
 *
 * That is what this structure records: one entry per committed frame, naming the
 * regions that frame wrote. "What must image X receive" then has a single answer —
 * the union of every entry newer than the epoch X last presented.
 *
 * Deliberately pure: no Vulkan, no allocation, no I/O. The window arithmetic is
 * the part of the mechanism most likely to be wrong, and it is the part that can
 * be proven by a table-driven unit test instead of a GPU run. `frame.c` and
 * `surface.c` own the images; this owns the policy.
 */
#ifndef FLUX_CORE_DAMAGE_HISTORY_H
#define FLUX_CORE_DAMAGE_HISTORY_H

#include <flux/core.h>
#include <flux/math.h> /* flux_rect */

/* How many frames of write history are retained. A swapchain rotation is
 * `minImageCount + 1` images deep in the worst case, so a deeper window than the
 * rotation is required for a steady-state frame to be describable; anything
 * deeper than this and the answer degrades to "copy everything", which is always
 * correct. */
#define FLUX_DAMAGE_HISTORY_DEPTH 8u

/* Sentinel epoch: "this image has never presented anything this surface's content
 * image could reproduce". Always resolves to a full copy. */
#define FLUX_DAMAGE_EPOCH_NONE UINT64_MAX

/* Upper bound on the rectangles one frame's write set may name. Mirrors the
 * public `FLUX_DAMAGE_CAPACITY`; a frame that exceeds it commits as `full`. */
#define FLUX_DAMAGE_HISTORY_RECTS 64u

typedef struct flux_damage_epoch {
    uint64_t epoch; /* 0 = this slot has never been written */
    flux_rect rects[FLUX_DAMAGE_HISTORY_RECTS];
    uint32_t count;
    bool full; /* the frame wrote its whole extent, or could not describe itself */
} flux_damage_epoch;

typedef struct flux_damage_history {
    flux_damage_epoch entries[FLUX_DAMAGE_HISTORY_DEPTH];
    /* Newest epoch that has an entry. Epochs are 1-based and strictly
     * increasing, so 0 doubles as "nothing committed yet". */
    uint64_t committed;
} flux_damage_history;

/* Forget every entry and restart the epoch counter. Called when the content
 * image is (re)created: after a resize the previous pixels are meaningless, so no
 * image is synchronised with anything. */
void flux_damage_history_reset(flux_damage_history *h);

/* Commit one frame's write set and return the epoch number it was given.
 *
 * `rects`/`count` may be empty, which means "this frame wrote its whole extent"
 * — the caller has no narrower description, and under-describing is the one
 * failure mode that leaves stale pixels on screen. `full` forces that reading
 * explicitly for a frame that could not fit its set in the bound. */
uint64_t flux_damage_history_commit(flux_damage_history *h, const flux_rect *rects, uint32_t count,
                                    bool full);

/* The regions `since`'s successor frames wrote, i.e. what an image last
 * synchronised at epoch `since` is missing.
 *
 * Returns the number of rectangles written to `out` (never more than `capacity`).
 * `*out_full` is set when the answer cannot be a list — the epoch predates the
 * retained window, an intervening frame was `full`, or the union exceeded
 * `capacity` — in which case the caller must copy the whole extent.
 *
 * `since == FLUX_DAMAGE_EPOCH_NONE` returns `*out_full = true`: never synchronised.
 * `since == h->committed` returns 0 with `*out_full == false`: the image is
 * already current. Those two answers are different facts and are never merged. */
uint32_t flux_damage_history_since(const flux_damage_history *h, uint64_t since, flux_rect *out,
                                   uint32_t capacity, bool *out_full);

#endif /* FLUX_CORE_DAMAGE_HISTORY_H */
