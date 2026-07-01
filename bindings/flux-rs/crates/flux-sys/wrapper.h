/*
 * Bindgen translation unit for the flux bindings.
 *
 * The per-module headers are included explicitly rather than via the
 * <flux/flux.h> umbrella: the umbrella gates canvas/scene/compute/effect
 * behind FLUX_HAVE_* defines that the build system sets for C consumers but
 * that are not carried in the pkg-config Cflags bindgen sees. Listing the
 * headers directly sidesteps the guards.
 *
 * The full public surface is bound. Text shaping is NOT here: it lives in
 * the flux-text sibling and is bound by `flux-text-sys` (ADR-0016).
 *
 * <flux/vulkan.h> pulls in <vulkan/vulkan.h>; bindgen's allowlist keeps the
 * generated output to flux_* plus only the Vk* types those signatures touch.
 */
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/compute.h>
#include <flux/core.h>
#include <flux/dmabuf.h>
#include <flux/effect.h>
#include <flux/math.h>
#include <flux/scene.h>
#include <flux/vulkan.h>
