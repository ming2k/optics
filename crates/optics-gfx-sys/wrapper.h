/*
 * Bindgen translation unit for the gfx bindings.
 *
 * The per-module headers are included explicitly rather than via the
 * <optics/gfx.h> umbrella so bindgen sees every public header directly.
 *
 * The full public surface is bound. Text shaping is NOT here: it lives in
 * the optics-text sibling and is bound by `optics-text-sys` (ADR-0016).
 *
 * <optics/gfx/vulkan.h> pulls in <vulkan/vulkan.h>; bindgen's allowlist keeps the
 * generated output to optics_gfx_* plus only the Vk* types those signatures touch.
 */
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/compute.h>
#include <optics/gfx/core.h>
#include <optics/gfx/curve.h>
#include <optics/gfx/dmabuf.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/math.h>
#include <optics/gfx/sdf.h>
#include <optics/gfx/vulkan.h>
