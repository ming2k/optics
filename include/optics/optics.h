/*
 * optics/optics.h — umbrella header for the Optics C23 stack (ADR-0160,
 * ADR-0164). It includes every installed library's entry header: gfx (Vulkan
 * RHI and 2D canvas), text, svg, scene, material, motion, window, ui and app.
 * The library graph is in docs/dev/standards/architecture.md (ADR-0166).
 *
 * Optional libraries are exposed only when the distribution's compile flags say
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
#define OPTICS_VERSION_PATCH 85

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

/* Document Layer: Vector Art & Static Graphics Compiler (svg, ADR-0161) */
#if defined(OPTICS_HAVE_SVG)
#include <optics/svg.h>
#endif

/* Platform shell: window, events, frame cadence (gfx only) */
#include <optics/window.h>

/* UI engine, and the application shell that runs it in a native window */
#if defined(OPTICS_HAVE_UI)
#include <optics/ui.h>
#endif
#if defined(OPTICS_HAVE_APP)
#include <optics/app.h>
#endif

#endif /* OPTICS_H */
