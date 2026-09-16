# Showcase Examples

Flagship composition and material demos built on Optics. Unlike per-library starter examples (`examples/<library>/`), these showcases demonstrate how multiple layers of the stack (Flux rendering + Prism materials + Lens UI + Iris native windowing) compose together to create modern desktop visuals with native Wayland cursor-shape and fractional HiDPI support.

```sh
meson setup build -Dexamples=true
meson compile -C build
./build/examples/showcase/material_gallery
```

| Demo | Kind | Description |
|------|------|-------------|
| [`material_gallery`](#material_gallery) | interactive, visual | All 4 Prism materials side-by-side: Liquid Glass, Frosted, Acrylic, Mica |

---

## material_gallery

The comparative visual showcase for the entire Optics Prism material library ([ADR-0096](../../docs/adr/0096-prism-mica-foundation-material.md), [ADR-0097](../../docs/adr/0097-canonical-example-taxonomy-and-governance.md)).
Renders an animated dynamic backdrop (radial color orbs, moving high-contrast
stripes, geometric rings) and overlays all four modern UI materials side-by-side:

1. **Liquid Glass** (`<prism/liquid_glass.h>`): physical IOR refraction,
   chromatic dispersion (RGB split), Fresnel specular rim lighting, and focus field.
2. **Frosted Glass** (`<prism/frosted.h>`): high-purity dual-Kawase blur with
   macOS-style vibrancy color saturation boost.
3. **Acrylic** (`<prism/acrylic.h>`): Windows Fluent-style dual-Kawase blur,
   luminance plate balancing (Smoke / Pearl), 1px SDF border highlight rim, and
   procedural blue-noise grain.
4. **Mica & Mica Alt** (`<prism/mica.h>`): Windows 11 desktop foundation material
   with screen-anchored wallpaper sampling, soft blur, theme tinting, mineral dither,
   and inactive fallback state.

### Interactive Hotkeys

- `1`: 4-Up Grid comparison view (all 4 materials side-by-side)
- `2`: Liquid Glass fullscreen focus
- `3`: Frosted Glass fullscreen focus
- `4`: Acrylic fullscreen focus
- `5`: Mica & Mica Alt fullscreen focus
- `D`: Toggle Dark / Light mode theme plate polarity (Smoke vs. Pearl)
- `F`: Toggle Inactive state fallback on Mica
- `Space`: Pause / Resume backdrop animation
- `Esc` or `Q`: Exit
