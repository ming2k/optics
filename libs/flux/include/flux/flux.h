/*
 * flux — C23 Vulkan-first graphics library.
 *
 * Umbrella header. Pulls in every module; libflux has no optional modules.
 * Consumers who want to keep their preprocessor narrow may
 * include the per-module headers directly:
 *
 *   <flux/core.h>     always available
 *   <flux/math.h>     always available
 *   <flux/curve.h>    always available (analytic curve kernel, ADR-0115)
 *   <flux/sdf.h>      always available (analytic distance fields, ADR-0118)
 *   <flux/observe.h>  always available (allocation ledger, ADR-0129)
 *   <flux/vulkan.h>   raw Vulkan handle accessors
 *   <flux/dmabuf.h>   iff built on Linux (FLUX_HAVE_DMABUF)
 *   <flux/canvas.h>   2D canvas (GPU and CPU backends)
 *   <flux/scene.h>    3D draw core
 *   <flux/compute.h>  compute pipelines
 *   <flux/effect.h>   image effects
 *
 * Text shaping is not in libflux: it lives in the glyph sibling
 * (`<glyph/glyph.h>`), which feeds flux_canvas_draw_glyph_run (ADR-0016, ADR-0106).
 */

#ifndef FLUX_H
#define FLUX_H

#include <flux/core.h>
#include <flux/curve.h>
#include <flux/math.h>
#include <flux/observe.h>
#include <flux/profile.h>
#include <flux/sdf.h>

#if defined(FLUX_HAVE_DMABUF)
#include <flux/dmabuf.h>
#endif

#include <flux/canvas.h>
#include <flux/canvas_helpers.h>
#include <flux/compute.h>
#include <flux/effect.h>
#include <flux/scene.h>

#endif /* FLUX_H */
