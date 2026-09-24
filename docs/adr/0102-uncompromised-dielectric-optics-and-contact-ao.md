---
id: ADR-0102
title: "Decoupled Dielectric Optics, Contact Ambient Occlusion, and Uncompromised Material Contrast"
status: accepted
date: 2026-09-18
scope: libs/prism, libs/lens, examples/showcase
superseded_by: null
negative_knowledge: true
---

# ADR-0102: Decoupled Dielectric Optics, Contact Ambient Occlusion, and Uncompromised Material Contrast

- Status: Accepted
- Date: 2026-09-18
- Scope: `libs/prism` (liquid_glass, acrylic, mica), `libs/lens` (material recipes, theme cascade), `examples/showcase` (material gallery).
- Depends on: [ADR-0046](0046-liquid-glass-physical-optics.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0065](0065-per-group-overrides-and-backdrop-stats.md), [ADR-0079](0079-layered-backdrop-material.md), [ADR-0096](0096-prism-mica-foundation-material.md), [ADR-0100](0100-liquid-glass-dual-rim-and-screen-space-environmental-optics.md), [ADR-0101](0101-physical-sampling-governor-and-bounded-material-dispatch.md).

## Context

Translucent and refractive materials in user interfaces face the fundamental **Two-Masters Dilemma**:
1. **Separation from Background:** Floating materials must clearly differentiate their boundaries from unpredictable backdrops, which range from HDR white (1.0) to dark scenes (0.0).
2. **Foreground Legibility:** Materials must provide a stable, legible contrast field for typography and controls placed on top of them.

Historically (ADR-0046, ADR-0065), the liquid-glass shader attempted to solve this by monolithically opposing the backdrop luminance—injecting dark smoke tint (`vec3(0.045, 0.05, 0.065)`) over bright backdrops and pearl tint over dark backdrops. However, in practice, this scheme suffered from critical architectural and visual shortcomings:

1. **The Light Theme Muddy Artifact:**
   Over bright or white backdrops, forcing the entire glass body to turn dark grey/smoke polluted the visual purity of light theme applications, creating an unnatural dirty/muddy appearance that clashed with surrounding light UI surfaces.
2. **The Foreground Inversion Conflict:**
   Light themes standardise on deep navy/black text (`#0A0F1D`). If a glass body dynamically darkens to oppose a white backdrop, the dark foreground text becomes illegible against the darkened glass. Conversely, pinning the polarity to Pearl (`1.0`) in light theme bypassed the adaptive detection entirely, causing white-on-white boundary evaporation.
3. **Shader/UI Boundary Disconnect:**
   Shaders in `acrylic.comp` and `mica.comp` hardcoded bright white specular rim highlights (`mix(vec3(1.0), vec3(0.9), plate)`), conflicting with the dark contact borders required in light mode.

## Decision

We establish an uncompromised physical model across `libs/prism` and `libs/lens` governed by **Contact Ambient Occlusion (CAO)**, **Isotropic Environmental Fresnel**, and **Purity of Dielectric Optics**:

### 1. Pure Dielectric Volumetric Absorption (No Smoke Plate Darkening)
Liquid glass is modelled as a true dielectric medium ($\text{IOR} \approx 1.52$). It does not artificially turn into dark sunglasses when placed over a white background. Tinting follows gentle volumetric absorption (Beer-Lambert law) scaled by `tint_strength`, eliminating dirty grey discoloration:
```glsl
float tint_amount = 0.12 * (0.65 + 0.35 * (1.0 - u)) * tint_strength;
colour = mix(colour, colour * tint_rgb, tint_amount);
```

### 2. Sub-Pixel Contact Ambient Occlusion (CAO)
Boundary separation over white/bright backdrops is achieved through physical contact shadow geometry rather than body darkening. At the silhouette boundary ($d \approx 0$), light is occluded at the micro-gap where the lens meets the surface, producing a razor-sharp $1.5\text{px}$ contact crease:
```glsl
float contact_alpha = clamp(contact_ao, 0.0, 1.0);
float contact_absorb = contact_alpha * 0.45 * exp(-d * d / 1.20);
colour *= 1.0 - contact_absorb;
```
This cleanly grounds white glass on a white table without polluting the interior body.

### 3. Continuous 360° Isotropic Environmental Fresnel Sheen
The rim lighting baseline is modulated by `ambient_fresnel`:
```glsl
float ambient_rim = (0.28 + 0.32 * max(ambient_fresnel, 0.0)) * line;
```
This ensures unbroken silhouette brilliance around all corners and caps regardless of directional light angle.

### 4. Theme-Aware Contact Rim Highlights in Acrylic and Mica
In `acrylic.comp` and `mica.comp`, the $1\text{px}$ SDF border rim transitions smoothly from a white specular highlight in dark mode (`luminance_plate = 0.0`) to a crisp dark slate contact rim (`vec3(0.12, 0.15, 0.20)`) in light mode (`luminance_plate = 1.0`):
```glsl
vec3 rim_col = mix(vec3(1.0), vec3(0.12, 0.15, 0.20), clamp(pc.luminance_plate, 0.0, 1.0));
tinted = mix(tinted, rim_col, border_intensity * pc.border_alpha);
```

### 5. Lens Theme and Recipe Integration
`lens_material_recipe` in `libs/lens/include/lens/material.h` exposes `contact_ao` and `ambient_fresnel`. `lens_theme_for_material` pairs light-mode material surfaces with dark slate contact borders (`0x181F2E`), providing immediate perceptual grounding.

## Rejected Alternatives

- **Dynamic Monolithic Body Darkening (Smoke over Bright):** Rejected. Turning the entire transparent body dark grey to separate it from a white background creates muddy, unappealing visuals and directly destroys contrast for dark foreground text.
- **CPU-Lagged Asynchronous Text Color Inversion:** Rejected. Reading backdrop luminance from the GPU to dynamically flip CPU text colors introduces 1–2 frames of latency, causing visible text color flicker when moving across luminance edges.
- **Pure Naked Blur Without Contact AO:** Rejected. Pure Gaussian/Kawase blur of a flat white background yields flat white; without micro contact occlusion, card boundaries completely evaporate on bright surfaces.

## Consequences

- Liquid glass bodies remain luminous, pure, and clean across both light and dark themes.
- Contact AO provides crisp, reliable silhouette boundaries even when placed on `#FFFFFF` backgrounds.
- Acrylic and Mica shaders seamlessly harmonize with Lens UI dark borders in light mode.
- All 136 unit, integration, and golden tests pass in lockstep across C, Rust bindings, and showcase applications.
