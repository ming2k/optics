/*
 * prism — the material library of the optics stack.
 *
 * Umbrella header. Materials are built on flux's public effect runtime;
 * flux itself knows nothing about any named material (ADR-0063).
 *
 * Architecture tiers:
 *   Tier 1: Base Material Layer (底衬材质层)
 *     - <prism/frosted.h>        classic non-distorting frosted blur material
 *     - <prism/acrylic.h>        acrylic material with procedural grain & luminance plate
 *     - prism_base_material      non-distorting foundation plates in multi-layer composites
 *
 *   Tier 2: Optical Glass Layer (高光与折射透镜层)
 *     - <prism/liquid_glass.h>   convex-lens optical glass, edge dispersion, diagonal
 *                                specular highlight pair, adaptive plate polarity
 *     - prism_optical_glass_params optical policy configuration
 *
 *   Tier 3: Multi-layering Compositor (多层级层叠复合材质)
 *     - <prism/backdrop_layer.h> single-dispatch compositor fusing Base Material Layer
 *                                beneath Optical Glass Layer to prevent backdrop bypass
 *
 * Common:
 *   - <prism/types.h>            common types, struct_type registry, versioning
 */

#ifndef PRISM_H
#define PRISM_H

#include <prism/acrylic.h>
#include <prism/backdrop_layer.h>
#include <prism/frosted.h>
#include <prism/liquid_glass.h>
#include <prism/mica.h>
#include <prism/types.h>

#endif /* PRISM_H */
