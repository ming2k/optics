/*
 * Internal compute-module surface. Never installed.
 */
#ifndef OPTICS_GFX_COMPUTE_INTERNAL_H
#define OPTICS_GFX_COMPUTE_INTERNAL_H

#include <optics/gfx/compute.h>

/* Internal: drop the pipeline's strong device reference and mark it
 * weak. For pipelines owned by per-device module state (e.g. the
 * effect module's blur pipeline): such state is torn down inside
 * optics_gfx_device_release, so a strong device ref would cycle and keep
 * the device refcount from ever reaching zero. Call only while the
 * caller itself holds a device reference. */
void optics_gfx_compute_pipeline_make_device_weak(optics_gfx_compute_pipeline *p);

#endif /* OPTICS_GFX_COMPUTE_INTERNAL_H */
