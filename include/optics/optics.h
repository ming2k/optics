/*
 * optics/optics.h — umbrella header for the Optics C23 stack (ADR-0160,
 * ADR-0164, ADR-0177). It includes every installed library's entry header:
 * gfx (Vulkan RHI and 2D canvas), text, window, ui (including header-only
 * <optics/ui/motion.h>), and app.
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
#define OPTICS_VERSION_PATCH 90

#define OPTICS_VERSION_NUMBER                                                                      \
    (((uint32_t)OPTICS_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_VERSION_MINOR << 8) |              \
     (uint32_t)OPTICS_VERSION_PATCH)

/* Rendering foundation: Vulkan 1.3 RHI, 2D canvas, compute, image effects */
#include <optics/gfx.h>

/* Typography & text shaping */
#if defined(OPTICS_HAVE_TEXT)
#include <optics/text.h>
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
