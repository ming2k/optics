/*
 * Internal cross-module helpers from src/math/colorspace.c, consumed by
 * the ICC parser (src/core/icc.c). Never installed.
 */
#ifndef OPTICS_GFX_COLORSPACE_INTERNAL_H
#define OPTICS_GFX_COLORSPACE_INTERNAL_H

#include <optics/gfx/math.h>

/* Bradford adaptation matrix mapping src_white to dst_white (XYZ). */
optics_gfx_mat3 optics_gfx_colorspace_adapt_xyz(optics_gfx_vec3 src_white,
                                                optics_gfx_vec3 dst_white);

/* RGB -> XYZ matrix for a valid color space (chromaticity-derived). */
optics_gfx_mat3 optics_gfx_colorspace_rgb_to_xyz(optics_gfx_color_space cs);

#endif /* FLUX_COLORSPACE_INTERNAL_H */
