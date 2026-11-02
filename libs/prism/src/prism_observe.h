/*
 * prism_observe.h — material filter accounting (ADR-0129, ADR-0134).
 *
 * Before this header, prism filters were invisible to the ledger. Their host
 * storage comes from calloc/free, so nothing counted the filter itself — only
 * the frame-slot images and pipelines it holds, which are flux objects and
 * therefore reported as `owner=flux/core`. A leaked prism filter was reported
 * as a flux leak: the wrong library, the wrong operator, and no way to tell a
 * material leak from a renderer one. Misattribution is worse than silence.
 *
 * Kinds are per-material ("filter_mica", "filter_liquid_glass", …) rather than
 * one "filter" kind with a material field, because the ledger's kind is the
 * unit a leak lane maps to and the unit a reader acts on: "the Mica filter
 * leaked" is actionable, "a filter leaked" is not.
 *
 * Scope is DEVICE (the filter holds a retained device and GPU objects), the
 * lifetime is CONTEXT (a filter outlives frames and dies with the app's
 * material set).
 */

#ifndef PRISM_OBSERVE_H
#define PRISM_OBSERVE_H

#include <flux/observe.h>
#include <prism/types.h>

#define PRISM_OBSERVE_OWNER "prism/material"

/* The ledger row is now written by flux_device_alloc itself, with
 * PRISM_OBSERVE_OWNER / these kinds (ADR-0135). Counting here as well would
 * double every prism allocation, which is exactly what happened when the
 * funnel was introduced — the lane caught it. The kind literals stay here
 * because they are the names both the funnel and the lane use. */

/* One literal per material so the guardrail's kind inventory and the call
 * sites cannot drift. */
#define PRISM_OBSERVE_KIND_MICA "filter_mica"
#define PRISM_OBSERVE_KIND_FROSTED "filter_frosted"
#define PRISM_OBSERVE_KIND_ACRYLIC "filter_acrylic"
#define PRISM_OBSERVE_KIND_LIQUID_GLASS "filter_liquid_glass"
#define PRISM_OBSERVE_KIND_BACKDROP_LAYER "filter_backdrop_layer"

#endif /* PRISM_OBSERVE_H */
