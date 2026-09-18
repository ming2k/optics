# ADR-0100: Liquid Glass dual-rim TIR and screen-space environmental optics

- Status: Accepted
- Date: 2026-09-18
- Scope: Compute shader (`libs/prism/src/shaders/liquid_glass.comp`), Prism material runtime, and showcase exhibits.
- Depends on: [ADR-0046](0046-liquid-glass-physical-optics.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0065](0065-per-group-overrides-and-backdrop-stats.md).

## Context

The initial implementation of `prism_liquid_glass` (ADR-0046, ADR-0063) established the baseline mathematical
model for real-time 2D UI glass: an analytical signed distance field (SDF) convex lens with inward normal displacement,
chromatic dispersion along the surface gradient, and an exterior key rim highlight.

Visual inspection against physical thick glass and state-of-the-art reference implementations (notably Apple VisionOS
and macOS spatial glass) revealed several critical physical discrepancies:

1. **Unilateral highlight and missing Total Internal Reflection (TIR):**
   The legacy shader evaluated highlights only on the light-facing silhouette (`facing = max(dot(n, key), 0.0)`).
   Conversely, the opposite silhouette (`away = max(dot(n, -key), 0.0)`) was penalized with an artificial dark shadow stroke
   (`colour *= 1.0 - 0.22 * pow(away, 1.8)`).
   In real dielectric thick glass ($n \approx 1.5$), incident light that penetrates the body travels through the medium
   and strikes the opposite curved meniscus at angles exceeding the critical angle ($\theta_c = \arcsin(1/1.5) \approx 41.8^\circ$).
   This induces **Total Internal Reflection (TIR)**, where the curved inner surface acts as a concave concentrator,
   producing a bright secondary caustic inner rim. Without this secondary highlight, UI elements appear flat,
   beveled, and devoid of volumetric physical thickness.

2. **Chalky monochromatic highlights (`vec3(1.0)`):**
   Highlights were hardcoded to pure unmodulated white, ignoring the color temperature, hue, and luminance of the scene.
   In reality, specular highlights represent reflected ambient environmental radiance. A flat white stroke reads as
   an artificial plastic overlay rather than polished glass embedded within a living environment.

3. **Background blending and vanishing hazards:**
   On uniform white or bright backdrops, translucent glass surfaces risked losing boundary legibility because
   diffuse transparency and highlights blended directly into the background without adequate edge attenuation.

## Decision

1. **Dual-Rim Physical Caustic Architecture & Pure Water-Droplet Model:**
   - Adopts the physical paradigm of a horizontal water droplet on a display screen viewed vertically from above.
   - Rejects artificial screen-space duplicate reflection overlays (`mix(colour, sample(uv + offset))`), eliminating
     unphysical double-image trailing/ghosting (重影/拖影). Transmitted screen light follows a single physical path.
   - **Primary Key Rim (Top-Left / Facing):** A sharp Gaussian highlight ($d \approx -0.65\text{px}$) modeling grazing
     Fresnel reflection on the outer front convex meniscus.
   - **Secondary TIR Back-Rim (Bottom-Right / Away):** An internal caustic line shifted inward ($d \approx -0.75\text{px}$),
     capturing total internal reflection and back-face caustic pooling.
   - Eliminates the artificial dark stroke on the away silhouette, replacing it with physical transmitted energy.

2. **Screen-Space Environmental Area Radiance Gathering (SS-IBL):**
   - Samples the pre-filtered blurred environment buffer (`pc.blurred_handle`) along the normal and key light directions
     with an adaptive radius (`env_step = max(edge * 1.6, 16.0) * inv_extent`).
   - Modulates both key and secondary specular highlights by the ambient chromatic radiance, blending a pure white
     specular core with scene color temperature:
     `key_spec_tint = mix(vec3(1.0), max(env_key, 0.12) * 1.5, 0.42)`.

3. **Four-Layer Boundary & Contrast Protection:**
   - **Silhouette Edge Absorption Trough:** Micro-absorption line right at $d \approx -0.35\text{px}$, strengthened over
     bright content (`(0.07 + 0.14 * (1.0 - dark_backdrop))`), guaranteeing that the glass perimeter never dissolves into white backdrops.
   - **Adaptive Luminance Balancing (Smoke vs. Pearl):** Dynamically tones down bright backdrops with a gentle smoke plate
     to maintain contrast for foreground content.
   - **Analytic Contact & Soft Drop Shadow:** SDF-derived drop shadow anchors the glass in space.
   - **Convex Lens Normal Displacement:** Strong geometric refraction bends underlying patterns, ensuring unambiguous boundary recognition.

## Alternatives Considered

- **Precomputed 3D Cubemap Environment Maps:** Rejected. UI elements exist in 2D/2.5D desktop compositing space where local
  screen-space backdrops are the authoritative source of ambient light. Sampling `blurred_handle` provides zero-allocation,
  zero-bandwidth overhead IBL.
- **Ray-marched screen-space caustics:** Rejected for UI real-time rendering. The analytic Gaussian inner-shift model ($d \approx -1.85\text{px}$)
  accurately matches the physical caustic focus of a cylindrical lens at a fraction of the GPU ALU cost.

## Consequences

- Liquid Glass elements instantly convey 3D tangible thickness and refractive depth across all orientations.
- Specular highlights naturally harmonize with dynamic scenes, colorful photography, and dark/light themes.
- Zero breaking changes to C API contracts or push constant binary layouts.
