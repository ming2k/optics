/*
 * optics/optics.h — canonical umbrella header for the Optics C23 stack.
 *
 * Provides a single unified entry point across all engine layers (ADR-0160,
 * ADR-0164):
 *   - Layer 0: Vulkan 1.3 RHI & 2D Vector Canvas (flux)
 *   - Layer 1: HarfBuzz Text Shaping & Typography (glyph)
 *   - Layer 2: Visual Capabilities, Material Optics (prism), Motion (transit), 3D (vista)
 *   - Document: SVG Static Art Compiler (reticle)
 *   - Layer 3: UI State Engine (lens) & Native Window Shell (iris)
 *
 * Optional layers are exposed only when the distribution's compile flags say
 * they are part of the installed stack (`OPTICS_HAVE_<MODULE>` from optics.pc).
 * Do not probe the host include path: a stale system install must not leak into
 * a build that deliberately omitted that module.
 */

#ifndef OPTICS_H
#define OPTICS_H

#include <stdint.h>

/* ================================================================== */
/*  Optics Stack Versioning                                           */
/* ================================================================== */

#define OPTICS_VERSION_MAJOR 0
#define OPTICS_VERSION_MINOR 0
#define OPTICS_VERSION_PATCH 82

#define OPTICS_VERSION_NUMBER                                                                      \
    (((uint32_t)OPTICS_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_VERSION_MINOR << 8) |              \
     (uint32_t)OPTICS_VERSION_PATCH)

/* Layer 0: Rendering Foundation & Rasterization Core */
#include <optics/gfx.h>

/* Layer 1: Typography & Text Shaping */
#if defined(OPTICS_HAVE_TEXT)
#include <optics/text.h>
#endif

/* Layer 2: Visual Capabilities & Material Optics */
#if defined(OPTICS_HAVE_MATERIAL)
#include <optics/material.h>
#endif
#if defined(OPTICS_HAVE_SCENE)
#include <optics/scene.h>
#endif

/* Layer 2: Motion Vocabulary (Header-Only, ADR-0158) */
#if defined(OPTICS_HAVE_MOTION)
#include <optics/motion.h>
#endif

/* Document Layer: Vector Art & Static Graphics Compiler (Reticle, ADR-0161) */
#if defined(OPTICS_HAVE_SVG)
#include <optics/svg.h>
#endif

/* Platform shell: window, events, frame cadence (flux only) */
#include <optics/window.h>

/* UI engine, and the application shell that runs it in an iris window */
#if defined(OPTICS_HAVE_UI)
#include <optics/ui.h>
#endif
#if defined(OPTICS_HAVE_APP)
#include <optics/app.h>
#endif

#endif /* OPTICS_H */
