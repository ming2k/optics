/*
 * gfx/observe_sites.h — fixed-capacity creation-site vocabulary (ADR-0129).
 *
 * Per-kind *file:line* sites at their primary creation sites, as interned
 * enum slots rather than `__FILE__ ":" __LINE__` strings. Two reasons, both
 * measured rather than stylistic:
 *
 *   1. A `__FILE__ ":" __LINE__` per counted call site drags a string per
 *      site into the shipped .so — measured 5 in a default gfx build before
 *      this header — which contradicted the ADR's own promise that a
 *      non-attribution build carries no site machinery.
 *   2. The string also lands in `.rodata` on the allocator hot path, where it
 *      is pure overhead: the ledger already knows the kind and owner, and the
 *      site's only job is to answer "where is this born?" for a reader.
 *
 * The vocabulary is fixed and closed, like the kind vocabulary: a new kind
 * either names one of these sites or takes OPTICS_GFX_SITE_NONE. Empty slots leave
 * no literal at all, so the default build is clean by construction.
 *
 * Full call-stack capture is not here by decision (ADR-0129 alternative 4): it
 * pulls libunwind/DbgHelp into Layer 0 and allocates on the allocator path.
 */

#ifndef OPTICS_GFX_OBSERVE_SITES_H
#define OPTICS_GFX_OBSERVE_SITES_H

#include <optics/gfx/core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum optics_gfx_site : uint16_t {
    OPTICS_GFX_SITE_NONE = 0,
    /* gfx core — host objects */
    OPTICS_GFX_SITE_HOST_ALLOC,  /* device.c: optics_gfx_internal_alloc */
    OPTICS_GFX_SITE_VK_MEMORY,   /* vk_allocator.c: optics_gfx_vk_allocate */
    OPTICS_GFX_SITE_VK_IMAGE,    /* canvas/image.c: optics_gfx_image_create */
    OPTICS_GFX_SITE_VK_BUFFER,   /* core/buffer.c: optics_gfx_buffer_create */
    OPTICS_GFX_SITE_VK_SAMPLER,  /* core/sampler.c: optics_gfx_sampler_create */
    OPTICS_GFX_SITE_VK_PIPELINE, /* core/graphics_pipeline.c: *_create */
    OPTICS_GFX_SITE_VK_TARGET,   /* core/target.c: optics_gfx_target_create */
    OPTICS_GFX_SITE_BINDLESS,    /* device.c: bindless slot pop */
    OPTICS_GFX_SITE_COUNT,
} optics_gfx_site;

/* The literal that owns this slot, or nullptr when the slot is unclaimed.
 * Reached through a function, not a static table, so the compiler emits
 * exactly the literals that are referenced. */
OPTICS_GFX_API const char *optics_gfx_site_label(optics_gfx_site site);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_OBSERVE_SITES_H */
