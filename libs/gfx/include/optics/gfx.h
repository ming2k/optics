/*
 * gfx — C23 Vulkan-first graphics library.
 *
 * Umbrella header. Pulls in every module; liboptics_gfx has no optional modules.
 * Consumers who want to keep their preprocessor narrow may
 * include the per-module headers directly:
 *
 *   <optics/gfx/core.h>     always available
 *   <optics/gfx/math.h>     always available
 *   <optics/gfx/curve.h>    always available (analytic curve kernel, ADR-0115)
 *   <optics/gfx/sdf.h>      always available (analytic distance fields, ADR-0118)
 *   <optics/gfx/observe.h>  always available (allocation ledger, ADR-0129)
 *   <optics/gfx/vulkan.h>   raw Vulkan handle accessors
 *   <optics/gfx/dmabuf.h>   iff built on Linux (OPTICS_GFX_HAVE_DMABUF)
 *   <optics/gfx/canvas.h>   2D canvas (GPU and CPU backends)
 *   <optics/gfx/scene.h>    3D draw core
 *   <optics/gfx/compute.h>  compute pipelines
 *   <optics/gfx/effect.h>   image effects
 *
 * Text shaping is not in liboptics_gfx: it lives in the glyph sibling
 * (`<optics/text.h>`), which feeds optics_gfx_canvas_draw_glyph_run (ADR-0016, ADR-0106).
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

#endif /* OPTICS_GFX_H */
