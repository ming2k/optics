/*
 * optics/optics.h — canonical umbrella header for the Optics C23 stack.
 *
 * Provides a single unified entry point across all engine layers (ADR-0160):
 *   - Layer 0: Vulkan 1.3 RHI & 2D Vector Canvas (flux)
 *   - Layer 1: HarfBuzz Text Shaping & Typography (glyph)
 *   - Layer 2: Visual Capabilities, Material Optics (prism), Motion (transit), 3D (vista)
 *   - Document: SVG Static Art Compiler (svg)
 *   - Layer 3: UI State Engine (lens) & Native Window Shell (iris)
 */

#ifndef OPTICS_H
#define OPTICS_H

#include <stdint.h>

/* ================================================================== */
/*  Optics Stack Versioning                                           */
/* ================================================================== */

#define OPTICS_VERSION_MAJOR 0
#define OPTICS_VERSION_MINOR 0
#define OPTICS_VERSION_PATCH 78

#define OPTICS_VERSION_NUMBER                                                                      \
    (((uint32_t)OPTICS_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_VERSION_MINOR << 8) |             \
     (uint32_t)OPTICS_VERSION_PATCH)

/* Layer 0: Rendering Foundation & Rasterization Core */
#include <flux/flux.h>

/* Layer 1: Typography & Text Shaping */
#if defined(__has_include)
#if __has_include(<glyph/glyph.h>)
#include <glyph/glyph.h>
#endif
#endif

/* Layer 2: Visual Capabilities & Material Optics */
#if defined(__has_include)
#if __has_include(<prism/prism.h>)
#include <prism/prism.h>
#endif
#if __has_include(<vista/vista.h>)
#include <vista/vista.h>
#endif
#endif

/* Layer 2: Motion Vocabulary (Header-Only, ADR-0158) */
#include <transit/transit.h>

/* Document Layer: Vector Art & Static Graphics Compiler (Reticle, ADR-0161) */
#if defined(__has_include)
#if __has_include(<reticle/reticle.h>)
#include <reticle/reticle.h>
#endif
#endif

/* Layer 3: UI State & Platform Shell */
#include <lens/lens.h>
#include <iris/iris.h>

#endif /* OPTICS_H */
