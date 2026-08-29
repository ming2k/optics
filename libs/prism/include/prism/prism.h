/*
 * prism — the material library of the optics stack.
 *
 * Umbrella header. Materials are built on flux's public effect runtime;
 * flux itself knows nothing about any named material (ADR-0063).
 *
 *   <prism/types.h>          common types and struct_type registry
 *   <prism/liquid_glass.h>   analytic liquid-glass material (convex-lens model)
 *   <prism/frosted.h>        classic non-distorting frosted glass material
 *   <prism/acrylic.h>        acrylic material with procedural grain & luminance plate
 *   <prism/backdrop_layer.h> layered backdrop compositor (frost + glass)
 */

#ifndef PRISM_H
#define PRISM_H

#include <prism/acrylic.h>
#include <prism/backdrop_layer.h>
#include <prism/frosted.h>
#include <prism/liquid_glass.h>
#include <prism/types.h>

#endif /* PRISM_H */
