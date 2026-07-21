# ADR-0034: Lens text measurement — host port + monospace fallback

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the measurement contract widgets
  rely on for intrinsic sizing.

## Context

Layout ([ADR-0028](0028-lens-flexbox-layout.md)) measures each leaf
before arrange, and many widgets (label, button, textfield, table cell)
need a text advance to pick their intrinsic size. Measurement must be
available during the build/measure pass, must respect the device-pixel
scale, and must produce a sensible answer when no font is loaded.

Forces:

1. **Accurate when a font is present.** Real shaping advance is needed
   so labels don't clip and tables don't mis-size columns.
2. **Graceful when no font.** Headless tests and CI may load no font;
   measurement must still return a non-zero, monotone-in-string-length
   answer.
3. **Weight and semantic size.** Buttons, titles, and headings use
   different sizes/weights; the API must accept both.
4. **Label convention.** The `"##key"` suffix is identity, not visible
   text; measurement must size only the visible prefix so the measured
   advance matches what is painted.

## Decision

1. **`lens_text_measure` / `lens_text_measure_ex`.** The public entry
   points take `(font, utf8, size_px)` and an optional `weight`
   (`_ex`). They route through `flux_text_measure` via the seam
   ([ADR-0033](0033-lens-text-seam.md)). Implemented in
   `libs/lens/src/text/seam.c`.
2. **`lensi_text_measure_label` for widgets.** Widgets call the internal
   helper that measures only the visible prefix of a label (everything
   before `"##"`), so the advance matches what the draw list will paint.
3. **Monospace fallback lives in flux-text.** When no shaping backend or
   font is available, the shared engine degrades internally to monospace
   metrics; lens never branches on backend presence.
4. **Layout pass 1 is the only caller during measure.** Leaves record
   their measured size during build; `measure()` in `solve.c` reads it
   back (overriding with `fixed_w`/`fixed_h` and clamping to
   min/max) when computing the intrinsic.
5. **Semantic text sizes.** `lens_theme` carries `font_size_title` /
   `font_size_h1` / `h2` / `h3` ([ADR-0032](0032-lens-theme-tokens.md));
   a zero value falls back to `font_size`. Title/heading widgets pass
   the resolved size into measurement.

References: `libs/lens/include/lens/lens.h` (Text seam section),
`libs/lens/src/text/seam.c`, `libs/lens/src/layout/solve.c`.

## Alternatives Considered

- **Measure the full label including `"##key"`.** Reject: the suffix is
  identity; measuring it would make painted text mis-aligned with the
  measured box.
- **Cache measurements per string.** Future work; current per-frame cost
  is acceptable at lens's scale, and caching would need invalidation on
  font/scale change.
- **Return only width; compute height/baseline elsewhere.** Reject: the
  struct return is cheaper than a second call and keeps baseline (needed
  for caret and icon alignment) co-located.

## Consequences

Positive:

- Widgets get accurate advances when a font is loaded and sane
  (monospace) advances when none is.
- The label-prefix convention is enforced in one place
  (`lensi_text_measure_label`), not re-derived per widget.

Negative:

- Monospace fallback underestimates proportional text; a UI that
  measures before the font loads will reflow one frame once the font
  arrives.
- Measurement runs every frame for every visible leaf; a future cache
  keyed by `(font, size_px, weight, utf8)` is the obvious follow-up.

## References

- [ADR-0028](0028-lens-flexbox-layout.md) — measure/arrange consumer.
- [ADR-0033](0033-lens-text-seam.md) — the seam measurement routes
  through.
- [ADR-0032](0032-lens-theme-tokens.md) — semantic text sizes.
