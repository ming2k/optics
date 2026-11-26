/*
 * Bindgen translation unit for the gfx bindings.
 *
 * The per-module headers are included explicitly rather than via the
 * <optics/gfx.h> umbrella: the umbrella gates canvas/scene/compute/effect
 * behind OPTICS_GFX_HAVE_* defines that the build system sets for C consumers but
 * that are not carried in the pkg-config Cflags bindgen sees. Listing the
 * headers directly sidesteps the guards.
 *
 * The full public surface is bound. Text shaping is NOT here: it lives in
 * the gfx-text sibling and is bound by `gfx-text-sys` (ADR-0016).
 *
 * <optics/gfx/vulkan.h> pulls in <vulkan/vulkan.h>; bindgen's allowlist keeps the
 * generated output to optics_gfx_* plus only the Vk* types those signatures touch.
 */
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/compute.h>
#include <optics/gfx/core.h>
#include <optics/gfx/dmabuf.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/math.h>
#include <optics/gfx/vulkan.h>
