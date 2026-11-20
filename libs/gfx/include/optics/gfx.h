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

#ifndef OPTICS_GFX_H
#define OPTICS_GFX_H

#include <optics/gfx/core.h>
#include <optics/gfx/curve.h>
#include <optics/gfx/math.h>
#include <optics/gfx/observe.h>
#include <optics/gfx/profile.h>
#include <optics/gfx/sdf.h>

#if defined(OPTICS_GFX_HAVE_DMABUF)
#include <optics/gfx/dmabuf.h>
#endif

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/compute.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/scene.h>

#endif /* FLUX_H */
