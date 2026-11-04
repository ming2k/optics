#ifndef PRISM_MATERIAL_FRAME_H
#define PRISM_MATERIAL_FRAME_H

/*
 * prism_material_frame.h — the per-frame machinery every prism material shares.
 *
 * Two mechanisms live here because all five materials (liquid glass, frosted,
 * acrylic, mica, layered backdrop) need them identically, and per-material
 * copies of either would drift:
 *
 *   1. SLAB GEOMETRY. A material's output image is sized to the material's own
 *      footprints on a coarse size ladder, not to the capture. A 4K window with
 *      a 48-px glass titlebar used to allocate 3 x 33 MiB of RGBA16F for a
 *      0.7 MiB need; sizing to the footprint is a 140x reduction in that case
 *      and also removes the per-frame reallocation a resize sweep caused.
 *
 *   2. BODY TABLE. A material's per-body parameters travel in a host-visible
 *      buffer addressed through a device address, not in the push-constant
 *      block. The push block is a hard 160 bytes (flux guarantees it, ADR-0063
 *      and ADR-0065 both hit the wall), it is already full for liquid glass, and
 *      it is per-dispatch state — so per-body state cannot live there without
 *      capping body count at one. Moving it to a buffer is what allows a single
 *      dispatch to cover every body in a region, which is what removes the
 *      per-body dispatch and the per-body whole-image barrier.
 *
 * Consequence for the public API: the returned image is slab-sized and its
 * content occupies a sub-rectangle. The placement travels with the image
 * (prism_material_surface) so the caller can draw it at the right place. That is
 * a deliberate break from "output extent == input extent": an image that claims
 * capture extent while holding a fraction of it is what forced the over-sized
 * allocation, and no amount of documentation makes that contract cheap.
 */

#include <flux/core.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Slab geometry                                                     */
/* ------------------------------------------------------------------ */

/* Smallest slab side. A slab never shrinks below this, so a tiny body does not
 * produce an un-dispatchable workgroup grid or a degenerate image. */
#define PRISM_SLAB_MIN_SIDE 256u

/* Size ladder step. Slabs are quantized up to a multiple of this on both axes,
 * so a window resize that walks a few pixels per frame reuses the same image
 * instead of releasing and re-creating one per step (measured: a 1-px-per-frame
 * sweep ran 1.7x slower than steady state purely from that churn). 128 keeps the
 * worst-case over-allocation under 2x on one axis while making resize free. */
#define PRISM_SLAB_QUANTUM 128u

/* Round `v` up to the ladder, with the floor. */
static inline uint32_t prism_slab_quantize(uint32_t v) {
    if (v < PRISM_SLAB_MIN_SIDE)
        v = PRISM_SLAB_MIN_SIDE;
    return (v + (PRISM_SLAB_QUANTUM - 1u)) / PRISM_SLAB_QUANTUM * PRISM_SLAB_QUANTUM;
}

/* The slab a footprint of `w` x `h` capture pixels needs. */
static inline void prism_slab_for_footprint(uint32_t w, uint32_t h, uint32_t *out_w,
                                            uint32_t *out_h) {
    *out_w = prism_slab_quantize(w);
    *out_h = prism_slab_quantize(h);
}

/* ------------------------------------------------------------------ */
/*  Body table                                                        */
/* ------------------------------------------------------------------ */

/* Hard ceiling on bodies a single material submission may carry. Also the
 * ceiling on rows the table allocates, so no submission can index past it.
 * Equal to the documented PRISM_*_MAX_GROUPS values; the public headers state
 * the number and the materials assert it matches. */
#define PRISM_MAX_BODIES 64u

/* Row stride in bytes for the body table.
 *
 * Deliberately NOT the per-body payload size: a fixed stride decoupled from the
 * row count means adding a body never moves any existing row, so a row's device
 * offset can never shift under an in-flight frame, and the shader's bound
 * dispatch can never read past what the host wrote. Every material's row layout
 * is static_asserted to fit. */
#define PRISM_BODY_STRIDE 256u

/* A host-visible, device-addressed array of per-body parameter rows.
 *
 * One table per filter. It is written during recording (the frame's own
 * sub-range of the table cannot be observed by another frame because a filter
 * slot is owned by one frame at a time) and read by the shader through the
 * 64-bit address. `flux devices do not guarantee shaderInt64`, so addresses
 * travel as two u32 push words and the shader rebuilds them with the
 * buffer-reference constructor — the same idiom backdrop_stats.comp uses. */
typedef struct prism_body_table {
    flux_buffer *buffer;
    uint8_t *mapped;
    uint64_t address;
    uint32_t capacity_rows;
} prism_body_table;

/* Declared but not created: creation is deferred to the first frame that needs
 * the table, so a filter that never renders a body never allocates one. */
#define PRISM_BODY_TABLE_INIT                                                                      \
    {.buffer = nullptr, .mapped = nullptr, .address = 0u, .capacity_rows = 0u}

/* Return the writable memory for `row`, creating or growing the table so it can
 * hold `needed_rows` rows. Growing re-creates the buffer, so callers must have
 * finished writing any host-side state that referenced the old address (it is
 * only ever referenced through the push block of the frame being recorded, and
 * that frame is not yet submitted when growth happens).
 *
 * Returns nullptr on allocation failure. */
static inline void *prism_body_table_row(flux_device *device, prism_body_table *table,
                                         uint32_t needed_rows, uint32_t row) {
    if (needed_rows > PRISM_MAX_BODIES || row >= needed_rows)
        return nullptr;
    if (!table->buffer || table->capacity_rows < needed_rows) {
        uint32_t capacity = needed_rows;
        if (table->buffer)
            flux_buffer_release(table->buffer);
        table->buffer = nullptr;
        table->mapped = nullptr;
        table->address = 0u;
        table->capacity_rows = 0u;
        flux_buffer_desc bdesc = FLUX_BUFFER_DESC_INIT;
        bdesc.size = (size_t)capacity * PRISM_BODY_STRIDE;
        bdesc.usage = FLUX_BUFFER_USAGE_STORAGE;
        bdesc.location = FLUX_BUFFER_HOST_VISIBLE;
        bdesc.device_address = true;
        if (flux_buffer_create(device, &bdesc, &table->buffer) != FLUX_OK) {
            table->buffer = nullptr;
            return nullptr;
        }
        void *mapped = flux_buffer_mapped(table->buffer);
        if (!mapped) {
            flux_buffer_release(table->buffer);
            table->buffer = nullptr;
            return nullptr;
        }
        table->mapped = mapped;
        table->address = flux_buffer_device_address(table->buffer);
        table->capacity_rows = capacity;
    }
    uint8_t *base = table->mapped + (size_t)row * PRISM_BODY_STRIDE;
    memset(base, 0, PRISM_BODY_STRIDE);
    return base;
}

static inline void prism_body_table_release(prism_body_table *table) {
    if (!table)
        return;
    if (table->buffer)
        flux_buffer_release(table->buffer);
    *table = (prism_body_table)PRISM_BODY_TABLE_INIT;
}

/* Address split for the push block (no shaderInt64). */
static inline uint32_t prism_address_lo(uint64_t address) {
    return (uint32_t)(address & 0xFFFFFFFFu);
}
static inline uint32_t prism_address_hi(uint64_t address) {
    return (uint32_t)(address >> 32u);
}

/* ------------------------------------------------------------------ */
/*  Region batch                                                      */
/* ------------------------------------------------------------------ */

/* A coalesced dispatch region plus the body ids it must evaluate.
 *
 * The host coalesces every footprint into disjoint rectangles (the same
 * transitive merge the clear pass already used) and, per rectangle, records
 * which bodies actually reach it. One dispatch then evaluates a rectangle
 * against its own short id list: bodies are batched, not broadcast, so a
 * region's shader work is proportional to the bodies that touch it and the
 * ~64-way scan a "loop over all bodies" design would pay is never needed.
 *
 * `ids_first` indexes into the submission's id array. Because the regions are
 * disjoint, two bodies that share pixels always land in the same region and are
 * ordered by their position in its id list — which is exactly the source-over
 * order the materials require. That is the second reason batching is safe: the
 * per-body barrier existed to order overlapping bodies, and merging them into
 * one dispatch orders them without a barrier at all. */
typedef struct prism_region_batch {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t ids_first;
    uint32_t id_count;
} prism_region_batch;

/* Type of the per-region id array the shader reads. Kept as u32 so the id array
 * and the body table can share one allocation strategy. */
typedef struct prism_body_ids {
    flux_buffer *buffer;
    uint8_t *mapped;
    uint64_t address;
    uint32_t capacity_ids;
} prism_body_ids;

#define PRISM_BODY_IDS_INIT                                                                        \
    {.buffer = nullptr, .mapped = nullptr, .address = 0u, .capacity_ids = 0u}

/* Writable id array for `needed_ids` entries. Same growth discipline as the body
 * table. Returns nullptr on failure. */
static inline uint32_t *prism_body_ids_slot(flux_device *device, prism_body_ids *ids,
                                            uint32_t needed_ids) {
    if (needed_ids == 0u)
        return nullptr;
    if (!ids->buffer || ids->capacity_ids < needed_ids) {
        uint32_t capacity = needed_ids;
        if (ids->buffer)
            flux_buffer_release(ids->buffer);
        ids->buffer = nullptr;
        ids->mapped = nullptr;
        ids->address = 0u;
        ids->capacity_ids = 0u;
        flux_buffer_desc bdesc = FLUX_BUFFER_DESC_INIT;
        bdesc.size = (size_t)capacity * sizeof(uint32_t);
        bdesc.usage = FLUX_BUFFER_USAGE_STORAGE;
        bdesc.location = FLUX_BUFFER_HOST_VISIBLE;
        bdesc.device_address = true;
        if (flux_buffer_create(device, &bdesc, &ids->buffer) != FLUX_OK) {
            ids->buffer = nullptr;
            return nullptr;
        }
        void *mapped = flux_buffer_mapped(ids->buffer);
        if (!mapped) {
            flux_buffer_release(ids->buffer);
            ids->buffer = nullptr;
            return nullptr;
        }
        ids->mapped = mapped;
        ids->address = flux_buffer_device_address(ids->buffer);
        ids->capacity_ids = capacity;
    }
    return (uint32_t *)ids->mapped;
}

static inline void prism_body_ids_release(prism_body_ids *ids) {
    if (!ids)
        return;
    if (ids->buffer)
        flux_buffer_release(ids->buffer);
    *ids = (prism_body_ids)PRISM_BODY_IDS_INIT;
}

/* ------------------------------------------------------------------ */
/*  Public-output plumbing                                            */
/* ------------------------------------------------------------------ */

/* The maximum number of coalesced regions a submission can produce. Every
 * footprint either merges into an existing region or adds one, so the count is
 * bounded by the footprint count; the largest footprint count of any material is
 * the layered backdrop's plates + bodies. 128 covers 64 bodies after the clear
 * pass doubles previous plus current, with headroom that the caller asserts
 * rather than assumes. */
#define PRISM_MAX_BATCHES 128u

/* ------------------------------------------------------------------ */
/*  Deferred pass registration (ADR-0147)                             */
/* ------------------------------------------------------------------ */

/* Every material's dispatches are deferrable, and every material needs the same
 * three steps: materialize the deferred inputs it samples, copy its dispatch
 * parameters into the frame's plan arena, register a pass that replays those
 * parameters later. Written once here because five private copies would drift —
 * the same reason the slab geometry and body table live in this header.
 *
 * Why a material cares at all: a material's dispatches are the most expensive
 * thing in a frame (measured 4.8 ms/frame for 64 liquid-glass bodies on a
 * 1024x1024 target), and a material whose output nothing samples should cost
 * nothing. Before this seam, registering was impossible from here: the plan was
 * a flux-internal header, so prism could only record eagerly.
 *
 * Contract, identical to flux's: registration happens at a pass boundary, and
 * the caller still returns the output handle immediately. The pass runs only if
 * something requires it — a downstream pass naming the token, or a host calling
 * flux_frame_require_pass. */

/* The record callback a material supplies. `params` is its own arena copy. */
typedef void (*prism_deferred_record_fn)(void *params, VkCommandBuffer cmd);

typedef struct prism_deferred_pass {
    /* Inputs the pass samples. A deferred (non-null) token is forwarded so the
     * plan can order itself after the pass that writes it; a null token means
     * the image comes from outside the plan. */
    flux_image *inputs[FLUX_DEFERRED_MAX_INPUTS];
    flux_deferred_token *input_tokens[FLUX_DEFERRED_MAX_INPUTS];
    uint32_t input_count;

    flux_image *output;
    const char *label;
} prism_deferred_pass;

#define PRISM_DEFERRED_PASS_INIT {.input_count = 0}

/* Register one material pass. The material's dispatch parameters are copied into
 * the frame's plan arena, so they may live on the caller's stack.
 *
 * Returns the pass's token through `out_token` when non-null. A null token is
 * legal — the pass then runs only if the host explicitly requires it.
 *
 * Errors are flux's own, unchanged: INVALID_ARGUMENT, INVALID_STATE (no pass
 * boundary), OUT_OF_MEMORY (arena full), OUT_OF_RANGE (plan full). */
static inline flux_result prism_deferred_register(flux_frame *frame,
                                                  const prism_deferred_pass *pass,
                                                  prism_deferred_record_fn record,
                                                  const void *params, size_t params_size,
                                                  flux_deferred_token **out_token) {
    if (!frame || !pass || !pass->output || !record)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (pass->input_count > FLUX_DEFERRED_MAX_INPUTS)
        return FLUX_ERROR_INVALID_ARGUMENT;

    flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
    desc.record = record;
    desc.params = (void *)params;
    desc.params_size = params_size;
    desc.label = pass->label;
    desc.input_count = pass->input_count;
    desc.output_count = 1u;
    desc.outputs[0] = pass->output;
    for (uint32_t i = 0; i < pass->input_count; ++i) {
        desc.inputs[i] = pass->inputs[i];
        desc.input_tokens[i] = pass->input_tokens[i];
    }
    return flux_frame_register_pass(frame, &desc, out_token);
}

/* Declare the pass live when its consumer is the host rather than another pass.
 * A null token is a no-op so callers need no branch. */
static inline flux_result prism_deferred_require(flux_frame *frame, flux_deferred_token *token) {
    if (!token)
        return FLUX_OK;
    return flux_frame_require_pass(frame, token);
}

#endif /* PRISM_MATERIAL_FRAME_H */
