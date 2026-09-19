# Liquid Glass Optical Architecture & Physical Rendering Paradigm

This document formalises the optical foundation, shader mathematics, contrast-preservation architecture, and automated verification methodology of **Prism Liquid Glass** (`<prism/liquid_glass.h>`, `liquid_glass.comp`). It details how Optics achieves physical dielectric glass realism for desktop and spatial user interfaces without ray tracing, adhering to [ADR-0100](../adr/0100-liquid-glass-dual-rim-and-screen-space-environmental-optics.md).

---

## 1. The Physics of Dielectric UI Materials

Traditional 2D user interfaces treat translucent surfaces as flat alpha-blended sheets with uniform blur (e.g. standard CSS `backdrop-filter: blur()`). While this creates a sense of layering, it lacks the optical depth, volumetric mass, and refractive tangibility of real physical glass.

A real solid glass plate with curved convex edges exhibits distinct optical phenomena:

```text
                             Incident Ambient Light (Key) ↘
                                             ┌─────────────────────────────────────┐
  [Primary Key Rim]                          │                                     │
  Grazing Fresnel Specular Reflection        │     Solid Dielectric Glass Medium   │
  Outer boundary (d ≈ -0.65px)               │          Index of Refraction        │
  White core + Ambient Hue Bleed             │               n ≈ 1.50              │
                                             │                                     │
                                             │       ↘ ↘ Refracted Transmitted ↘ ↘ │
                                             └─────────────────────────────────────┘
                                                                                    [Secondary TIR Back-Rim]
                                                                                    Total Internal Reflection
                                                                                    Inner Meniscus (d ≈ -0.75px)
                                                                                    Caustic Focus & Ground Bounce
```

### 1.1 The Pure Water-Droplet-on-Screen Paradigm
The physical model is straightforward and pure: **a water droplet resting horizontally on a transparent glass display, with an eye looking straight down from the vertical $Z$-axis**.
- **Single-path transmission:** The display underneath emits light upward. Rays travel through the dielectric body once.
- **No duplicate reflection ghosting:** The glass surface does not reflect a duplicate shifted copy of the screen below it. Artificial screen-space reflection overlays (`mix(colour, sample(uv + offset))`) are explicitly rejected because they introduce unphysical double-image trailing (重影).
- **Physical lens magnification:** The flat center passes light perpendicularly with zero distortion, while the curved meniscus perimeter refracts underlying lines inward in an organic S-curve with chromatic dispersion.

### 1.2 Snell's Law & Refractive Displacement
When light crosses the boundary between air ($n_1 = 1.0$) and glass ($n_2 \approx 1.5$), it refracts according to Snell's law:
$$n_1 \sin\theta_1 = n_2 \sin\theta_2$$
Along the curved perimeter of the glass, the surface normal $\mathbf{n}$ tilts rapidly. This creates a cylindrical lens that magnifies and displaces the underlying background inward toward the center of the surface:
$$\mathbf{p}_{\text{sample}} = \mathbf{p} - \mathbf{n} \cdot \Delta_{\text{refract}}(d)$$
where $d$ is the signed distance to the silhouette boundary.

### 1.3 Chromatic Dispersion (Prismatic Split)
Because glass exhibits chromatic dispersion (Cauchy's equation: $n(\lambda) = A + B/\lambda^2$), blue light bends more sharply than red light. Prism models this by separating the red, green, and blue sampling coordinates along the outward surface gradient:
$$\mathbf{p}_r = \mathbf{p} - \mathbf{n} \cdot (\Delta + \delta_c), \quad \mathbf{p}_g = \mathbf{p} - \mathbf{n} \cdot \Delta, \quad \mathbf{p}_b = \mathbf{p} - \mathbf{n} \cdot (\Delta - \delta_c)$$
This confines prismatic color fringing strictly to the curved rim band where light bends, keeping the flat interior 100% legible and distortion-free.

---

## 2. The 3D Meniscus Illumination & Dual-Rim Architecture

A signature visual cue of physical dielectric glass (e.g. VisionOS and macOS spatial glass) is the presence of **highlights on both sides and continuous structural definition across all four corners**:

### 2.1 Overhead Key Illumination (Top Edge & Top Corners)
In realistic ambient environments, overhead diffuse ceiling light illuminates the entire upper curved meniscus ($-n_y > 0$). Modulated smoothly by the incident key light direction $\mathbf{l}_{\text{key}}$, both top-left and top-right corners maintain consistent brilliance without abrupt cutoffs:
$$I_{\text{top}} = (-n_y)^{1.25} \cdot \left(0.65 + 0.35 \cdot \max(\mathbf{n} \cdot \mathbf{l}_{\text{key}}, 0)^{1.25}\right) \cdot \exp\left(-\frac{(d + 0.65)^2}{2.00}\right)$$

### 2.2 Secondary TIR Caustic Field (Bottom Edge & Bottom Corners)
Light entering the droplet medium propagates to the opposite boundary ($n_y > 0$). When propagating from glass ($n_1 \approx 1.5$) to air ($n_2 = 1.0$), angles exceeding the critical angle ($\theta_c \approx 41.8^\circ$) undergo **Total Internal Reflection (TIR)**. This produces a secondary caustic rim shifted slightly deeper into the glass wall, beautifully illuminating both bottom-left and bottom-right corners:
$$I_{\text{bot}} = (n_y)^{1.25} \cdot \left(0.55 + 0.35 \cdot \max(\mathbf{n} \cdot -\mathbf{l}_{\text{key}}, 0)^{1.25}\right) \cdot \exp\left(-\frac{(d + 0.80)^2}{2.00}\right)$$

### 2.3 360° Continuous Meniscus Fresnel Baseline
To ensure capsule caps and orthogonal corners never suffer from blackout dropouts, a solid physical baseline wraps the entire perimeter:
$$I_{\text{base}} = 0.38 \cdot \exp\left(-\frac{(d + 0.65)^2}{2.00}\right)$$

### 2.4 Screen-Space Environmental Area Radiance Gathering (SS-IBL)
Specular highlights reflect local ambient scene radiance rather than artificial flat white:
$$\text{spec\_tint} = \text{mix}\left(\text{vec3}(1.0), \max(\text{frost}, 0.12) \times 1.35, 0.28\right)$$

### 2.5 Engineering Sweet Spot Benchmark (甜蜜点工程基准)
- **Four-Corner Highlight Ratio:** **$1.30 : 1 \sim 1.40 : 1$** (TL: 0.99, TR: 0.82, BR: 0.92, BL: 0.76), eliminating historical 9.3:1 diagonal blackouts.
- **Per-Pixel Sampling Footprint:** Exactly 4 taps for Liquid Glass (3 chromatic refraction + 1 environmental frost); exactly 1 tap for Frosted/Acrylic/Mica.
- **Metric Distance Invariant:** G2 squircle distance field enforced via Taubin first-order gradient normalization $\|\nabla d\| = 1.0$ at all angles.

---

## 3. Contrast Preservation & Boundary Defense

A fundamental challenge in spatial glass design is preventing translucent elements from vanishing when placed over similarly-colored or uniform white backdrops. Prism enforces a **four-layer defense hierarchy**:

| Layer | Mechanism | Physical Purpose |
| :--- | :--- | :--- |
| **1. Edge Absorption Trough** | Micro-dark absorption line at $d \approx 0\text{px}$ | Acts as an optical razor cut; strengthens on bright backdrops so glass never dissolves into white. |
| **2. Adaptive Luminance Plate** | Smoke / Pearl plate modulation based on backdrop Rec.709 luminance | Tones down high-luminance backgrounds ($Y > 0.5$) with a soft dimming veil to preserve text legibility. |
| **3. Analytic Drop Shadow** | Dual-radius SDF shadow (contact shadow + soft ambient penumbra) | Physically lifts the glass plane above the canvas, establishing a clear spatial depth gap. |
| **4. Cylindrical Lens Distortion** | Inward normal displacement of underlying geometry | Bends and breaks background lines/patterns at the boundary, providing unambiguous geometric proof of glass. |

---

## 4. Automated Verification & Golden Regression Methodology

Real-time shader algorithms cannot be verified reliably by manual inspection alone. Optics establishes a rigorous, headless, and reproducible verification methodology:

```text
┌─────────────────────────┐     ┌─────────────────────────┐     ┌─────────────────────────┐
│ 1. Headless GPU Device  │ ──> │ 2. Deterministic Scene  │ ──> │ 3. Sub-Pixel DMA Buffer │
│    flux_device_create   │     │    Fixed seed & extent  │     │    flux_surface_read_px │
└─────────────────────────┘     └─────────────────────────┘     └─────────────────────────┘
                                                                             │
                                ┌─────────────────────────┐                  ▼
                                │ 5. Compiler Isolation   │     ┌─────────────────────────┐
                                │    Tracked .h headers   │ <── │ 4. Tolerance Budgeting  │
                                │    Bypasses ccache trap │     │    max_delta & outliers │
                                └─────────────────────────┘     └─────────────────────────┘
```

1. **Headless Vulkan Surface Pipeline:** Tests run on headless GPU surfaces (`flux_device_desc.headless = true`), isolating rendering from window managers, Wayland compositors, and HiDPI DPI-scaling variations.
2. **Sub-Pixel DMA Readback:** Frame output is retrieved directly from VRAM via `flux_surface_read_pixels` for mathematical channel assertions.
3. **Statistical Outlier & Maximum Delta Budgeting:** Acknowledging minor driver floating-point rounding variations across GPU hardware (Intel ANV, AMD RADV, Nvidia proprietary), golden comparisons enforce a strict two-tier budget:
   - `max_delta <= 12` per channel for allowable rounding variance;
   - `outliers <= total * 0.08%` for boundary rasterization antialiasing tolerances;
   - Structural regressions immediately trigger test failures.
4. **Automated Golden Reference Update:** Reference images (`tests/prism/golden/liquid_glass_default.pam`) are committed to version control and updated via explicit `PRISM_GOLDEN_UPDATE=1` invocation upon intentional shader upgrades.
5. **Compiler Header Isolation:** To prevent stale shader object cache hits (e.g. GCC 16 C23 `#embed` failing to emit depfile entries), shader bytecode is embedded via explicit Ninja-tracked header targets (`liquid_glass_spv.h`), ensuring every shader modification rebuilds dependent libraries deterministically.

---

## 5. Related Architecture Decisions

- [ADR-0046: Liquid Glass physical optics model](../adr/0046-liquid-glass-physical-optics.md)
- [ADR-0063: Liquid Glass material library](../adr/0063-liquid-glass-material-library.md)
- [ADR-0096: Prism Mica foundation material](../adr/0096-prism-mica-foundation-material.md)
- [ADR-0100: Liquid Glass dual-rim TIR and screen-space environmental optics](../adr/0100-liquid-glass-dual-rim-and-screen-space-environmental-optics.md)
