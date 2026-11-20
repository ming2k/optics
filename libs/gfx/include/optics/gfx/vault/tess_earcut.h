/*
 * flux/vault/tess_earcut.h — Pure Ear-Clipping Polygon Tessellator Asset (ADR-0121).
 *
 * Part of the Algorithm Vault: a pure, stateless computational geometry library.
 * Fully decoupled from canvas context, graphics pipeline, device state, and UI.
 *
 * Preserved for offline mesh extraction, 3D extrusion, navmesh/collider generation,
 * and deterministic CPU differential verification.
 *
 * Invariant: Zero dynamic allocation during triangulation when caller provides
 * scratch storage. Thread-safe, pure arithmetic.
 */

#ifndef OPTICS_GFX_VAULT_TESS_EARCUT_H
#define OPTICS_GFX_VAULT_TESS_EARCUT_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration and output for ear-clip triangulation */
typedef struct optics_gfx_vault_earcut_desc {
    const optics_gfx_point *points; /* Input vertices */
    uint32_t point_count;           /* Number of vertices in the polygon contour */
    uint32_t *out_indices;          /* Output buffer for triangle indices (3 * (point_count - 2)) */
    uint32_t index_capacity;        /* Max indices out_indices can hold */
} optics_gfx_vault_earcut_desc;

/* Result metrics for diagnostic and verification */
typedef struct optics_gfx_vault_earcut_result {
    uint32_t triangle_count; /* Number of triangles produced */
    uint32_t index_count;    /* Total indices emitted (3 * triangle_count) */
    bool is_clockwise;       /* Detected contour orientation */
} optics_gfx_vault_earcut_result;

/* Triangulate a simple polygon (no self-intersections) using ear-clipping.
 * Returns FLUX_OK on success, or error code on invalid parameters / degenerate input. */
OPTICS_GFX_API optics_gfx_result optics_gfx_vault_earcut_triangulate(
    const optics_gfx_vault_earcut_desc *desc, optics_gfx_vault_earcut_result *out_result);

/* Compute exact polygon signed area (Gauss shoelace formula). */
OPTICS_GFX_API float optics_gfx_vault_polygon_signed_area(const optics_gfx_point *pts, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_VAULT_TESS_EARCUT_H */
