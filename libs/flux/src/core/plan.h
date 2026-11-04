/*
 * plan.h — the frame's deferred pass plan (ADR-0147).
 *
 * Recording an effect used to mean recording its compute dispatches, in
 * order, into the frame's main command buffer. That made every effect call
 * unconditional GPU work: the engine had no way to know that nothing would
 * ever sample the result, because the sample is emitted later by an
 * unrelated module — or never.
 *
 * A pass is now *registered* instead of recorded. Registration is pure
 * bookkeeping: the caller receives its output image handle immediately and
 * every input/output is retained by the frame exactly as before. Dispatches
 * are built into a secondary command buffer only when a consumer requires
 * the output, and only passes that reached a consumer are ever executed.
 *
 * The ordering rule is one sentence: **build on require, execute on flush,
 * and flush only at a pass boundary.** Vulkan forbids compute inside a
 * render pass, so a boundary is the only place a secondary may be executed;
 * it is also, by construction, before any later pass can sample the result.
 * A consumer that needs the pixels while it records its own compute (a
 * material filter) calls flux_frame_materialize_image, which requires and
 * flushes at its own boundary — so the producer is always ahead of the
 * consumer in the command stream.
 *
 * Pruning is decided by absence of readers, never by a heuristic: a pass no
 * consumer required cannot affect a presented pixel, so its body is never
 * built and its secondary never executed. Nothing is allocated for it beyond
 * the slot record itself.
 *
 * Everything lives on the frame: fixed-capacity pass slots plus a bump arena
 * for parameters, with secondaries coming from the frame slot's own command
 * pool (reset by begin_frame after that slot's fence was waited). No heap
 * allocation and no graph compilation on the hot path.
 */
#ifndef FLUX_CORE_PLAN_H
#define FLUX_CORE_PLAN_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct flux_image;

/* Deferred passes one frame can hold. Effects compose in ones and twos per
 * surface (a backdrop capture blur, then a material that samples it); the
 * ceiling exists so the plan is a fixed array rather than a growable one. */
#define FLUX_PLAN_MAX_PASSES 64u

/* Images one pass may declare as read / written. */
#define FLUX_PLAN_MAX_PASS_INPUTS 8u
#define FLUX_PLAN_MAX_PASS_OUTPUTS 2u

/* Bump arena for pass parameters, paid once per frame. Sized for the largest
 * descriptor a pass copies (a layered backdrop: 16 base plates plus 64 glass
 * groups) with headroom. */
#define FLUX_PLAN_ARENA_BYTES (48u * 1024u)

/* Generational pass token (ADR-0147).
 *
 * Carrying the epoch prevents ABA aliasing across frame slot recycling: a
 * token from 3 frames ago (same slot index, same pass array address) carries
 * an older epoch and is rejected as stale rather than silently aliasing this
 * frame's pass. */
struct flux_deferred_token {
    uint32_t pass_index;
    uint64_t epoch;
};

/* Builds the pass's dispatches into its own command buffer. Must leave every
 * output readable by a compute sampling stage; barriers land in the same
 * submission, so the caller needs no extra synchronisation. */
typedef void (*flux_plan_record_fn)(void *params, VkCommandBuffer cmd);

/* What the caller hands to flux_plan_register. */
typedef struct flux_plan_pass_desc {
    flux_plan_record_fn record;
    void *params;      /* lives in the plan arena (flux_plan_alloc_params) */
    const char *label; /* debug label for the frame's pass scope */

    /* Images this pass reads, each with the explicit token of the pass that
     * writes it (nullptr for an image no pass produces). The token is stated
     * rather than inferred from the image pointer so a pass can order itself
     * after an unrelated pass, and so two passes writing one image cannot
     * silently claim each other's output. */
    struct flux_image *inputs[FLUX_PLAN_MAX_PASS_INPUTS];
    const struct flux_deferred_token *input_tokens[FLUX_PLAN_MAX_PASS_INPUTS];
    uint32_t input_count;

    struct flux_image *outputs[FLUX_PLAN_MAX_PASS_OUTPUTS];
    uint32_t output_count;
} flux_plan_pass_desc;

/* One registered pass. */
typedef struct flux_plan_pass {
    flux_plan_record_fn record;
    void *params;
    const char *label; /* debug label emitted around the pass body, never owned */

    /* Resolved prerequisites, in two kinds:
     *
     *   provider[i] — the pass that writes input i, or UINT32_MAX when the
     *                 image comes from outside the plan. Builds before us.
     *   requires[]  — passes this one declared as a prerequisite via a token
     *                 without also sampling the image. Same effect on liveness.
     *
     * Both are resolved at registration and only ever point at *earlier*
     * passes, which is why no cycle guard exists: an edge to a later pass is
     * impossible by construction, so the dependency relation is a DAG of
     * bounded depth and the recursive build cannot loop. */
    uint32_t provider[FLUX_PLAN_MAX_PASS_INPUTS];
    uint32_t provider_count;

    uint32_t
        requires[
            FLUX_PLAN_MAX_PASS_INPUTS];
    uint32_t requires_count;

    struct flux_image *outputs[FLUX_PLAN_MAX_PASS_OUTPUTS];
    uint32_t output_count;

    VkCommandBuffer segment; /* owned by the frame slot's pool */
    bool required;           /* a consumer asked for this pass's output */
    bool built;              /* dispatches are recorded into `segment` */
    bool executed;           /* vkCmdExecuteCommands was recorded for it */
} flux_plan_pass;

typedef struct flux_plan {
    flux_plan_pass passes[FLUX_PLAN_MAX_PASSES];
    struct flux_deferred_token tokens[FLUX_PLAN_MAX_PASSES];
    uint32_t pass_count;
    uint64_t epoch;

    struct flux_device *device; /* for debug labels; not retained */
    VkDevice vk_device;
    VkCommandPool pool; /* the frame slot's pool, already reset this frame */

    alignas(16) unsigned char arena[FLUX_PLAN_ARENA_BYTES];
    size_t arena_used;

    uint32_t executed_total; /* read by flux_frame_plan_stats */
    uint32_t fallback_total; /* passes that fell back to eager execution */
} flux_plan;

/* Empty the plan and adopt the frame slot's command pool. Called by
 * begin_frame after that slot's fence was waited and its pool reset, so no
 * secondary the previous plan allocated is still executing. */
void flux_plan_reset(flux_plan *p, struct flux_device *device, VkCommandPool pool);

/* Copy pass parameters out of caller stack memory into the plan's arena.
 * Returns nullptr when the arena is exhausted, so the caller can fall back
 * to recording eagerly instead of registering a pass it cannot rebuild. */
void *flux_plan_alloc_params(flux_plan *p, size_t bytes, size_t alignment);

/* Register a pass and return its index, or UINT32_MAX when the plan is full.
 * Nothing touches the command buffer here: the body is built on require. */
uint32_t flux_plan_register(flux_plan *p, const flux_plan_pass_desc *desc);

/* Mark a pass required by index without sampling anything — the seam for a host
 * that consumes an output by drawing it. Idempotent. */
void flux_plan_require_index(flux_plan *p, uint32_t index);

/* Translate a pass index into the opaque token handed to callers, and back.
 * The token *is* the slot address, so no side table is needed and a stale token
 * from a previous frame cannot silently address a different pass. */
struct flux_deferred_token *flux_plan_token(flux_plan *p, uint32_t index);
uint32_t flux_plan_token_index(const flux_plan *p, const struct flux_deferred_token *token);

/* Mark every producer `image` depends on as required and build their bodies.
 * Idempotent. Returns false only when a required pass could not be built.
 * Does not execute: execution belongs to flux_plan_flush, at a boundary. */
bool flux_plan_require_image(flux_plan *p, struct flux_image *image);

/* True when `image` is a registered pass output that has not been executed
 * yet. Draw paths use it to reject sampling an unwritten image rather than
 * presenting stale or blank pixels. */
bool flux_plan_image_pending(const flux_plan *p, const struct flux_image *image);

/* Execute every required, built, not-yet-executed pass in registration order
 * at the given boundary. Idempotent, so repeated flushes cost one array
 * scan. Must be called with no render pass active. */
void flux_plan_flush(flux_plan *p, VkCommandBuffer cmd);

/* Flush, then count the passes that never reached a consumer. Returns the
 * number pruned this frame. Called at submit, after the last boundary. */
uint32_t flux_plan_finalize(flux_plan *p, VkCommandBuffer cmd);

#endif /* FLUX_CORE_PLAN_H */
