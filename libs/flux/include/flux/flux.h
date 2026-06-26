/*
 * flux — C23 Vulkan-first graphics library.
 *
 * Umbrella header. Pulls in every module enabled at build time.
 * Consumers who want to keep their preprocessor narrow may
 * include the per-module headers directly:
 *
 *   <flux/core.h>     always available
 *   <flux/math.h>     always available
 *   <flux/vulkan.h>   raw Vulkan handle accessors
 *   <flux/canvas.h>   iff built with -Dcanvas=true
 *   <flux/dmabuf.h>   iff built with -Dcanvas=true
 *   <flux/scene.h>    iff built with -Dscene=true
 *   <flux/compute.h>  iff built with -Dcompute=true
 *   <flux/effect.h>   iff built with -Deffect=true
 *
 * Text shaping is not in libflux: it lives in the flux-text sibling
 * (`<flux-text/text.h>`), which feeds flux_canvas_draw_glyph_run (ADR-0016).
 */

#ifndef FLUX_H
#define FLUX_H

#include <flux/core.h>
#include <flux/math.h>

#if defined(FLUX_HAVE_CANVAS)
#include <flux/canvas.h>
#include <flux/dmabuf.h>
#endif

#if defined(FLUX_HAVE_SCENE)
#include <flux/scene.h>
#endif

#if defined(FLUX_HAVE_COMPUTE)
#include <flux/compute.h>
#endif

#if defined(FLUX_HAVE_EFFECT)
#include <flux/effect.h>
#endif

#endif /* FLUX_H */
