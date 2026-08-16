# ADR-0070: ICC profile support — in-tree C parser over vendored skcms

- Status: Accepted
- Date: 2026-08-16

## Context

ADR-0069 milestone 4 calls for ICC support and named skcms as the
vehicle: "a vendored, single-purpose ICC parser (skcms — MIT, two
source files, the same library Chrome/Skia use)". During implementation
that choice collided with a harder constraint: libflux is a pure C23
library ("Pure C" is a public header promise in `<flux/core.h>`), the
build has no C++ toolchain, and skcms is C++. Vendoring it would add a
C++ compiler requirement and a libstdc++ (or equivalent) linkage
question to a library whose embedding story (static linking, Rust
bindgen, exotic compositor hosts) depends on staying trivially
linkable. The slice of ICC that flux actually needs is bounded and
well-specified (ICC.1:2010 v2/v4, RGB display-class profiles), so the
trade-off changed.

## Decision

Flux carries an **in-tree C ICC parser** (`libs/flux/src/core/icc.c`)
implementing the bounded subset the render pipeline consumes:

- ICC v2/v4 header + tag table; display-class RGB profiles.
- Matrix profiles: `rXYZ/gXYZ/bXYZ`, `wtpt`, `chad` (v4 D50
  adaptation), `rTRC/gTRC/bTRC` as `curveType` gamma or
  `parametricCurveType` 0–4.
- LUT profiles: `A2B0` as `mft1` (lut8) / `mft2` (lut16) / `mAB`
  (curves + matrix + CLUT), evaluated once on the CPU at load time and
  baked into a 65³ RGBA16F 3D-LUT image. Lab PCS is converted to XYZ
  (D50) during evaluation.
- Extraction is two-tier per ADR-0069: matrix+TRC profiles whose
  curves match flux's transfer set (pure gamma, or the exact sRGB
  parametric constants) become a parametric `flux_color_space`;
  everything else becomes the baked 3D LUT. Both slot into the same
  image-content color path (a per-image GPU params block referenced
  from the image draw's push constants).

The parametric/LUT split, the working-space-first architecture, and
the "ICC is a source of transforms, not a transform engine" stance of
ADR-0069 are unchanged; this ADR only replaces the parser vehicle.

## Alternatives Considered

- **Vendor skcms (ADR-0069 as written).** Rejected here: breaks the
  pure-C build (new toolchain + C++ runtime linkage for a
  system-library-grade .so) to solve a problem whose in-tree cost is
  ~1000 lines with full test coverage against synthetic profiles.
- **Vendor lcms2 (subproject).** Rejected (as in ADR-0069): it is a
  full CPU CMM; flux would use it only as a parser/baker, paying the
  whole dependency for a fraction of its surface, and its dynamic
  plugin/transform machinery is exactly the historical weight this
  design avoids.
- **No ICC at all; named spaces only.** Rejected: camera/photo/print
  content arrives ICC-tagged in the wild; without a parser the color
  story silently degrades for precisely the users who asked for it.

## Consequences

- `libs/flux/src/core/icc.c` is the only ICC code; no new external
  dependency, no C++ in the build.
- New public surface: `flux_icc_profile` (opaque, device-free parse),
  `flux_icc_profile_color_space` query, and
  `flux_image_color_space_desc` on `flux_image_desc.next`
  (FLUX_TYPE_IMAGE_COLOR_SPACE_DESC). `docs/reference/symbols.md` and
  the glossary gain the entries in the same commit.
- The image draw's push block gains a color-params bindless handle
  (reusing a padding field; the 160-byte budget is unchanged).
- iccMAX, printer/scanner device classes, and soft proofing remain out
  of scope (as ADR-0069 ruled).
- If the subset proves insufficient in the field (exotic mAB
  variants), the fallback is extending icc.c — not adopting a CMM.

## References

- ADR-0069 (color management architecture)
- ICC.1:2010 (ICC v4.3), ICC v2 specification (tag/type formats)
- skcms (Google) — design evidence that the subset suffices for a
  GPU renderer
