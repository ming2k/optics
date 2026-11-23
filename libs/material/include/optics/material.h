/*
 * material — the material library of the optics stack.
 *
 * Umbrella header. Materials are built on gfx's public effect runtime;
 * gfx itself knows nothing about any named material (ADR-0063).
 *
 * Architecture tiers:
 *   Tier 1: Base Material Layer (底衬材质层)
 *     - <optics/material/frosted.h>        classic non-distorting frosted blur material
 *     - <optics/material/acrylic.h>        acrylic material with procedural grain & luminance plate
 *     - optics_material_base_material      non-distorting foundation plates in multi-layer
 * composites
 *
 *   Tier 2: Optical Glass Layer (高光与折射透镜层)
 *     - <optics/material/liquid_glass.h>   convex-lens optical glass, edge dispersion, diagonal
 *                                specular highlight pair, adaptive plate polarity
 *     - optics_material_optical_glass_params optical policy configuration
 *
 *   Tier 3: Multi-layering Compositor (多层级层叠复合材质)
 *     - <optics/material/backdrop_layer.h> single-dispatch compositor fusing Base Material Layer
 *                                beneath Optical Glass Layer to prevent backdrop bypass
 *
 * Common:
 *   - <optics/material/types.h>            common types, struct_type registry, versioning
 */

#ifndef OPTICS_MATERIAL_H
#define OPTICS_MATERIAL_H

#include <optics/material/acrylic.h>
#include <optics/material/backdrop_layer.h>
#include <optics/material/frosted.h>
#include <optics/material/liquid_glass.h>
#include <optics/material/mica.h>
#include <optics/material/types.h>

#endif /* OPTICS_MATERIAL_H */
