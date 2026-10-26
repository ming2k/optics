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

#ifndef FLUX_VAULT_TESS_EARCUT_H
#define FLUX_VAULT_TESS_EARCUT_H

#include <flux/core.h>
#include <flux/math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration and output for ear-clip triangulation */
typedef struct flux_vault_earcut_desc {
    const flux_point *points; /* Input vertices */
    uint32_t point_count;     /* Number of vertices in the polygon contour */
    uint32_t *out_indices;    /* Output buffer for triangle indices (3 * (point_count - 2)) */
    uint32_t index_capacity;  /* Max indices out_indices can hold */
} flux_vault_earcut_desc;

/* Result metrics for diagnostic and verification */
typedef struct flux_vault_earcut_result {
    uint32_t triangle_count; /* Number of triangles produced */
    uint32_t index_count;    /* Total indices emitted (3 * triangle_count) */
    bool is_clockwise;       /* Detected contour orientation */
} flux_vault_earcut_result;

/* Triangulate a simple polygon (no self-intersections) using ear-clipping.
 * Returns FLUX_OK on success, or error code on invalid parameters / degenerate input. */
FLUX_API flux_result flux_vault_earcut_triangulate(const flux_vault_earcut_desc *desc,
                                                   flux_vault_earcut_result *out_result);

/* Compute exact polygon signed area (Gauss shoelace formula). */
FLUX_API float flux_vault_polygon_signed_area(const flux_point *pts, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_VAULT_TESS_EARCUT_H */
