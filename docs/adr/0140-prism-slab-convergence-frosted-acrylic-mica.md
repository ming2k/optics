---
id: ADR-0140
title: "Prism slab convergence: migrating Frosted, Acrylic, and Mica to footprint-quantized slabs and eliminating material memory asymmetry"
status: accepted
date: 2026-11-02
superseded_by: null
scope: libs/prism, architecture
negative_knowledge: true
---

# ADR-0140: Prism slab convergence: migrating Frosted, Acrylic, and Mica to footprint-quantized slabs

- Status: Accepted
- Date: 2026-11-02
- Scope: `libs/prism/src/{frosted,acrylic,mica}.c`, `libs/prism/src/shaders/{frosted,acrylic,mica}.comp`, `libs/prism/src/prism_material_frame.h`.
- Implementation: Landed. Closes the asymmetry documented in ADR-0139. Frosted, Acrylic, and Mica now allocate output images sized strictly to their submission footprints via `prism_slab_for_footprint` on the 128-px quantised ladder, passing `slab_origin_x/y` and `capture_width/height` down to their compute shaders.
- Depends on: [ADR-0139](0139-footprint-sized-material-slabs-and-batched-dispatch.md), [ADR-0063](0063-liquid-glass-material-library.md).
- Completes: the "Remaining asymmetry" clause of ADR-0139.

---

## Context and Problem Statement

ADR-0139 established the uniform return contract `prism_material_output` across all five physical surface materials in `prism`. It migrated `liquid_glass` and `backdrop_layer` to footprint-quantized slabs on a 128-px ladder with a 256-px floor (`prism_material_frame.h`).

However, the migration left an architectural asymmetry recorded in ADR-0139's consequences:

> **Remaining asymmetry:** frosted, acrylic and mica still allocate capture-sized slots and return a zero origin; their slab migration is a per-material change with no API impact, because the `prism_material_output` return is uniform.

This asymmetry produced three concrete defects:

1. **Misleading Contract**: Callers receiving `prism_material_output` assumed all materials benefited from footprint-sized memory savings. In reality, opening a 200x50 frosted toolbar or acrylic sidebar on a 4K window still allocated 3 x 33 MiB (RGBA8) or 3 x 66 MiB (RGBA16F) of capture-sized output images across frame slots.
2. **Resize Churn**: Window resizes continued to reallocate capture-sized images for frosted, acrylic, and mica every frame, causing frame drops during interactive window dragging.
3. **Architectural Incompleteness**: Leaving 3 of 5 materials on legacy full-screen allocation violated the "no half-migrations, zero historical baggage" mandate.

## Architectural Decision

All three remaining materials (`frosted`, `acrylic`, `mica`) are migrated to footprint-sized slabs using `prism_slab_for_footprint` from `prism_material_frame.h`.

### 1. Shader Push-Constant Alignment

The push-constant blocks of `frosted.comp`, `acrylic.comp`, and `mica.comp` are extended by 16 bytes:
```glsl
    float slab_origin_x; /* slab top-left in capture pixels */
    float slab_origin_y;
    float capture_width; /* full capture extent for UV normalization */
    float capture_height;
```

Inside the shaders:
- `local + origin_in_slab` indexes pixels within `u_storage` (`[0..width-1, 0..height-1]`).
- `p = vec2(pixel) + 0.5 + vec2(slab_origin_x, slab_origin_y)` converts invocation coordinates into capture coordinates.
- Sampling UVs for `input` and `blurred_input` are computed as `p / vec2(capture_width, capture_height)`.
- SDF evaluations (`rounded_rect_sdf(p, bounds, radius)`) evaluate `p` against capture-space `bounds`.
- Drop-shadow and dither evaluations use capture coordinates, ensuring visual continuity under slab translation.

Push block sizes remain well within Vulkan guarantees:
- `frosted_push`: 92 → 108 bytes (<= 128 bytes).
- `acrylic_push`: 100 → 116 bytes (<= 128 bytes).
- `mica_push`: 132 → 148 bytes (<= 160 bytes guaranteed by Flux).

### 2. Driver-Side Slab Lifecycle

In `frosted.c`, `acrylic.c`, and `mica.c`:
1. **Footprint Union**: Group dispatch bounds are aggregated in capture coordinates to compute `[footprint_x0, footprint_y0, footprint_x1, footprint_y1]`.
2. **Slab Sizing**: `prism_slab_for_footprint(footprint_w, footprint_h, &width, &height)` determines the quantized extent. Reallocation occurs only when the quantized extent changes, making incremental resizes free.
3. **Coordinate Remapping**: Regions dispatched to the shader and clear passes have their origins translated by `-slab_x, -slab_y`.
4. **Slab Invalidation**: If the slab origin shifts between frames while extent is unchanged, the entire slab is cleared transparent to prevent stale trails.
5. **Output Delivery**: The filter populates `prism_material_output` with the active slab image and `(origin_x, origin_y, width, height)`.

## Consequences

- **100% Convergence**: All five Prism materials (`liquid_glass`, `backdrop_layer`, `frosted`, `acrylic`, `mica`) share identical slab geometry, ladder quantization, and placement contracts.
- **VRAM Reductions**: Frosted, acrylic, and mica memory consumption on 4K displays drops from ~100-200 MiB to <15 MiB for typical UI chrome surfaces.
- **Zero API Impact**: Because ADR-0139 proactively unified the return type to `prism_material_output`, no caller or binding code breaks.
