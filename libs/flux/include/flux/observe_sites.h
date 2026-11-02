/*
 * flux/observe_sites.h — fixed-capacity creation-site vocabulary (ADR-0129).
 *
 * Per-kind *file:line* sites at their primary creation sites, as interned
 * enum slots rather than `__FILE__ ":" __LINE__` strings. Two reasons, both
 * measured rather than stylistic:
 *
 *   1. A `__FILE__ ":" __LINE__` per counted call site drags a string per
 *      site into the shipped .so — measured 5 in a default flux build before
 *      this header — which contradicted the ADR's own promise that a
 *      non-attribution build carries no site machinery.
 *   2. The string also lands in `.rodata` on the allocator hot path, where it
 *      is pure overhead: the ledger already knows the kind and owner, and the
 *      site's only job is to answer "where is this born?" for a reader.
 *
 * The vocabulary is fixed and closed, like the kind vocabulary: a new kind
 * either names one of these sites or takes FLUX_SITE_NONE. Empty slots leave
 * no literal at all, so the default build is clean by construction.
 *
 * Full call-stack capture is not here by decision (ADR-0129 alternative 4): it
 * pulls libunwind/DbgHelp into Layer 0 and allocates on the allocator path.
 */

#ifndef FLUX_OBSERVE_SITES_H
#define FLUX_OBSERVE_SITES_H

#include <flux/core.h>

typedef enum flux_site : uint16_t {
    FLUX_SITE_NONE = 0,
    /* flux core — host objects */
    FLUX_SITE_HOST_ALLOC,  /* device.c: flux_internal_alloc */
    FLUX_SITE_VK_MEMORY,   /* vk_allocator.c: flux_vk_allocate */
    FLUX_SITE_VK_IMAGE,    /* canvas/image.c: flux_image_create */
    FLUX_SITE_VK_BUFFER,   /* core/buffer.c: flux_buffer_create */
    FLUX_SITE_VK_SAMPLER,  /* core/sampler.c: flux_sampler_create */
    FLUX_SITE_VK_PIPELINE, /* core/graphics_pipeline.c: *_create */
    FLUX_SITE_VK_TARGET,   /* core/target.c: flux_target_create */
    FLUX_SITE_BINDLESS,    /* device.c: bindless slot pop */
    FLUX_SITE_COUNT,
} flux_site;

/* The literal that owns this slot, or nullptr when the slot is unclaimed.
 * Reached through a function, not a static table, so the compiler emits
 * exactly the literals that are referenced. */
FLUX_API const char *flux_site_label(flux_site site);

#endif /* FLUX_OBSERVE_SITES_H */
