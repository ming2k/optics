/*
 * Internal cross-module helpers from src/math/colorspace.c, consumed by
 * the ICC parser (src/core/icc.c). Never installed.
 */
#ifndef FLUX_COLORSPACE_INTERNAL_H
#define FLUX_COLORSPACE_INTERNAL_H

#include <flux/math.h>

/* Bradford adaptation matrix mapping src_white to dst_white (XYZ). */
flux_mat3 flux_colorspace_adapt_xyz(flux_vec3 src_white, flux_vec3 dst_white);

/* RGB -> XYZ matrix for a valid color space (chromaticity-derived). */
flux_mat3 flux_colorspace_rgb_to_xyz(flux_color_space cs);

#endif /* FLUX_COLORSPACE_INTERNAL_H */
