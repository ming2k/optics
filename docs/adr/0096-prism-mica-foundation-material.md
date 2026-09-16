# ADR-0096: Prism Mica foundation material — screen-anchored wallpaper composite

- Status: Accepted
- Date: 2026-09-15
- Scope: New material component in `libs/prism` (`<prism/mica.h>`), compute shaders (`src/shaders/mica.comp`), and filter implementation (`src/mica.c`).
- Depends on: [ADR-0063](0063-liquid-glass-material-library.md).

## Context

The Optics material library (`libs/prism`) currently ships three dynamic backdrop materials:
`prism_liquid_glass` (optical refraction and lens distortion), `prism_frosted` (vibrant dual-Kawase blur),
and `prism_acrylic` (multi-layer noise-dithered translucent surface with luminance plate balancing).
All three materials sample live, per-frame dynamic backdrops captured from the surface or desktop
immediately behind the window.

While live backdrop sampling delivers rich transient visuals for popups, menus, sidebars, and control panels,
it is sub-optimal as a foundation background for long-lived, large application windows:
1. **GPU power consumption:** Continuously capturing and filtering the region behind large application
   windows consumes significant fill rate and memory bandwidth every frame, even when the underlying desktop
   is static.
2. **Screen-anchoring expectation:** In modern desktop design languages (notably Windows 11 Fluent Design),
   the primary application backdrop is expected to incorporate the user's desktop wallpaper anchored
   to absolute screen coordinates. As a window moves across the screen, the material subtly reveals the portion
   of the wallpaper behind the window without per-frame desktop capture costs.
3. **Hierarchy and Alt variants:** Applications require a distinct visual foundation hierarchy between
   the window root background and commanding/tabbed headers (e.g. Mica Base vs. Mica Alt).

WinUI 3 and Windows 11 formalized this as the **Mica material** (`BuildMicaEffectBrush`), combining
screen-aligned wallpaper sampling, moderate blur, luminance plate balancing (Pearl/Smoke), theme tinting,
procedural dithering, and inactive state fallbacks.

## Decision

1. **New first-class material in `libs/prism`: Mica (`<prism/mica.h>`).**
   Ships `prism_mica_filter` with standard Optics resource lifecycle conventions:
   `prism_mica_filter_create`, `prism_mica_filter_retain`, `prism_mica_filter_release`, and
   `prism_mica_filter_apply`.

2. **WinUI-faithful effect pipeline in `mica.comp`:**
   - **Screen-anchored or local UV coordinate evaluation:** When `screen_width > 0` and `screen_height > 0`,
     the wallpaper is sampled using absolute desktop screen coordinates `(pixel + screen_origin) / screen_dimensions`.
     When zero, it falls back to local surface UVs.
   - **Luminosity plate balancing:** Blends the sampled wallpaper against a Pearl (`vec3(0.95)`) or
     Smoke (`vec3(0.12)`) plate controlled by `luminosity_plate` [0, 1] and `luminosity_opacity`.
   - **Color tinting:** Blends theme tint color and opacity over the balanced plate.
   - **Mica Base and Mica Alt (`PRISM_MICA_BASE` / `PRISM_MICA_ALT`):** Mica Alt provides deeper tinting
     and higher contrast for commanding bars and tab strips.
   - **Procedural blue-noise / triangular dither:** Prevents 8-bit banding across soft gradients.
   - **1px subtle SDF border highlight & analytic drop shadow:** Consistent with Fluent and Prism conventions.
   - **Inactive fallback blend:** Seamlessly blends toward a solid `fallback_color` when the window is inactive
     or when low-power mode is requested (`fallback_weight`).

3. **Type registry update:**
   `PRISM_TYPE_MICA_DESC = 5` is appended to `prism_struct_type` in `<prism/types.h>`.

4. **Storage formats:**
   Compiled for both RGBA8 UNORM and RGBA16 SFLOAT (`PRISM_STORAGE_RGBA16F`).

## Alternatives Considered

- **Emulate Mica via `prism_acrylic` with low blur:**
  Rejected. Acrylic is fundamentally tied to live per-frame backdrop capture and lacks screen-space
  wallpaper coordinate anchoring, resulting in unnecessary continuous GPU capture overhead for static windows.
- **Implement Mica in `lens` widget skins:**
  Rejected. ADR-0063 established that all named materials, shader pipelines, and optical identities
  belong exclusively in `libs/prism`. Lens only manages UI layout and geometry.

## Consequences

- Applications can render high-performance, battery-friendly foundation window surfaces that respect
  desktop wallpaper alignments and dark/light system themes.
- Zero runtime overhead when unused; standard frame-slot safety and pipeline caching apply.
