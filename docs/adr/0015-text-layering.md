# ADR-0015: Text layering — Layer-0 shaping in libflux, Layer-1 layout in flux-text-layout

- Status: Superseded by [ADR-0016](0016-pure-rhi-and-draw-primitives.md)
- Date: 2026-06-22

## Context

[ADR-0001](0001-project-foundations.md) listed text shaping under
"Out of scope, deliberately":

> Text shaping. Belongs in a sibling library wrapping HarfBuzz.

[ADR-0010](0010-glyph-blit-primitive.md) reinforced that boundary: the
library would own only the batched draw primitive, and "the library
does not own fonts, shaping, atlas packing, or atlas eviction. Sibling
library boundary from ADR-0001 holds."

Two things changed since:

1. **flux was reorganised for v0.1.** The separate `flux-text` library
   was merged *into* libflux as the `text` module (`<flux/text.h>`),
   built on FreeType + HarfBuzz + Fontconfig + FriBidi (gated by
   `-Dshaping`). README's Design Rules defend this on the Skia analogy:
   "Text rendering (`-Dshaping`) is part of the engine: like Skia's
   `SkShaper` / `SkParagraph`, it ships with the rendering library."
2. **The `text` header draws an explicit Layer-0 / Layer-1 line.**
   Layer-0 (single-run shaping) is implemented; Layer-1
   (`flux_text_layout`, paragraph composition with wrapping) is
   "reserved, not yet defined — do not assume its shape."

Meanwhile the Rust safe wrapper `flux` had grown a `Text::wrap` method —
Layer-1 paragraph logic — *inside the engine bindings crate*, ahead of
the C engine defining Layer-1. That collapsed the boundary the engine
itself drew, and made a provisional helper part of flux's API contract.

## Decision

A two-layer text model, mirrored across the C library and the Rust
crates:

### Layer 0 — shaping, in libflux

The `text` module (`<flux/text.h>`, `-Dshaping`) owns
FreeType/HarfBuzz/Fontconfig/FriBidi, the glyph atlas, BiDi-correct
caret/selection mapping, and single-run shape/measure/draw. **This
supersedes the "sibling library" boundary in ADR-0001 and ADR-0010**:
shaping is now an in-tree module of the rendering library, like Skia's
`SkShaper`. ADR-0010's glyph-blit primitive
(`flux_canvas_draw_glyph_run`) remains and is what Layer-0 draws
through — the primitive is unchanged, only the "shaping stays out"
stance is retired.

### Layer 1 — layout, in a separate crate

Paragraph composition (line wrapping, and the future multi-run
retained layout) lives in the **`flux-text-layout`** Rust crate, *not*
in libflux and *not* in the `flux` bindings crate. It is pure
composition over `flux::text::Text::measure` — no FFI of its own, no
Vulkan, no canvas access. The C header still reserves
`flux_text_layout` ("not yet defined"); the Rust crate previews that
surface without committing the C engine to it. When the C
`flux_text_layout` lands, the Rust crate can wrap it or be superseded.

### The `flux` bindings crate mirrors only Layer-0

`flux::text::Text` exposes shape / measure / draw / caret / selection —
the C engine's actual surface. It does **not** ship paragraph layout;
doing so would expand flux's API contract into `SkParagraph` territory
while the engine deliberately defers it.

## Consequences

Positive:

- The C library's text surface matches its own documented Layer-0 /
  Layer-1 split. No drift between the header's "reserved" note and
  what the bindings ship.
- Consumers get in-tree shaping (one library, one link line) without
  the engine being committed to paragraph layout. The Layer-0/Layer-1
  split is the same one Skia draws between `SkShaper` and `SkParagraph`.
- Layer-1 helpers can evolve in their own crate (versioned, breakable
  independently) without a library-wide API churn. When the C
  `flux_text_layout` is defined, the migration is one crate, not the
  engine surface.

Negative:

- Two Rust crates instead of one for text. Consumers wanting wrapping
  add `flux-text-layout` to `Cargo.toml`. Mitigated: it is a thin,
  single-dependency crate.
- The C `flux_text_layout` reservation means the eventual C Layer-1
  surface could differ from what `flux-text-layout` chose. The crate's
  docstring is explicit that its signature is provisional and may move
  down into C.
- ADR-0001 and ADR-0010 are partially superseded (their text-shaping
  boundary). Recorded inline on those ADRs; the rest of each stands.

## Alternatives considered

- **Keep shaping out, in a sibling library (the original ADR-0001
  stance).** Rejected: every consumer targeting flux for UI already
  brings FreeType/HarfBuzz; a separate library multiplies the
  link/config story for no isolation benefit, and the `SkShaper`
  analogy the README invokes puts shaping in the engine.
- **Ship Layer-1 (`wrap`) inside the `flux` bindings crate.** Rejected
  (the state this ADR corrects): it made `flux::text::Text::wrap` part
  of the engine's API contract while the C header said Layer-1 was
  "not yet defined," and locked a provisional signature in place.
- **Ship Layer-1 as a C module now.** Rejected as premature: paragraph
  layout (UAX#14 break iteration, multi-run paragraphs, justification)
  is a larger design than the engine needs to commit to today. The
  pure-Rust crate lets the design settle without C ABI exposure.

## See also

- [ADR-0001](0001-project-foundations.md) — project foundations
  (text-shaping scope boundary superseded by this ADR).
- [ADR-0010](0010-glyph-blit-primitive.md) — glyph-blit primitive
  (the "shaping stays out" stance is superseded; the primitive
  remains and is what Layer-0 draws through).
- `README.md` — Design Rules (the `SkShaper` / `SkParagraph`
  analogy this layering follows).
- `include/flux/text.h` — the Layer-0 / Layer-1 split this ADR
  records.
- The Layer-1 Rust crate `flux-text-layout` lives in the
  [`flux-rs`](https://github.com/ming2k/flux-rs) repository.
