# ADR-0032: Lens theme token system — sized struct with ABI guard

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the theme data model.

## Context

Lens widgets need a shared set of design tokens (colours, spacing,
radii, typography) so the toolkit looks consistent and so a host can
swap themes (light/dark, system theme) without per-widget plumbing. The
theme struct is part of the public API (`lens_desc.theme`,
`lens_set_theme`), so its memory layout is an ABI surface.

Forces:

1. **Forward compatibility.** Apps compiled against an older `lens_theme`
   must keep working when lens adds new tokens; apps compiled against a
   newer header must not overwrite memory when lens is older.
2. **Normalised defaults.** Many tokens are optional (zero = inherit /
   fallback); the toolkit must apply defaults so a caller can pass a
   mostly-zeroed theme.
3. **Rich enough for real UI.** Scrollbars, sliders, accent rails, and
   semantic text sizes each need their own tokens.

## Decision

1. **Single `lens_theme` struct, leading `size` field.** The struct
   opens with `uint32_t size;` set to `sizeof(lens_theme)`. A zero `size`
   means "trust the full struct" (legacy); a non-zero `size` lets the
   library clamp copies to `min(caller, lib)` so older or newer binaries
   degrade cleanly without an ABI break. Same pattern as
   [ADR-0036](0036-lens-input-clipboard-ime.md).
2. **Token groups.** The struct carries colour tokens (`color_bg`,
   `color_fg`, `color_accent`, `color_border`, `color_hover`,
   `color_active`, `color_disabled`, `color_error`), spacing tokens
   (`padding`, `gap`, `corner_radius`, `border_width`), typography tokens
   (`font`, `font_size`, semantic title/h1/h2/h3 sizes, weight tokens),
   and widget-specific groups: `active_indicator_width`, scrollbar
   styling (width/radius/min-thumb-h + track/thumb colours), and slider
   styling (track thickness/knob size + track/fill/knob colours).
3. **Two built-in palettes.** `lens_theme_default` (light) and
   `lens_theme_dark` ship with the library.
4. **Normalisation at every entry point.** `lensi_theme_normalize`
   (called from `lens_create` and `lens_set_theme`) fills zeroed tokens
   with derived defaults: semantic font sizes derive from `font_size`;
   scrollbar/slider colours inherit from `color_border` /
   `color_accent` / `color_fg`; geometry floors (scrollbar width,
   slider knob) default to fixed px values.
5. **Per-call overrides still possible.** Widget descriptor forms
   (`lens_button_opts`, `lens_tabs_opts`, …) take explicit colours/
   radii that fall back to the theme when zero.

References: `libs/lens/include/lens/lens.h` (Theme tokens section),
`libs/lens/src/theme/theme.c`, `libs/lens/src/core/context.c`
(`lensi_theme_normalize`).

## Alternatives Considered

- **Theme as a string-keyed map.** Reject: string lookup per widget per
  frame is wasteful, and the struct gives static field access and
  inlining.
- **Separate structs per token group.** Reject: callers want one object
  to pass and persist; a flat struct is simpler.
- **Hard-coded defaults, no normalisation.** Reject: every widget would
  re-derive defaults; centralised normalisation keeps behaviour
  consistent.
- **Version sentinels (e.g. `version = 2`).** Reject: the `size` guard
  is finer-grained (per-byte) and matches the Vulkan `pNext` idiom.

## Consequences

Positive:

- Adding tokens is ABI-forward-compatible; old binaries keep working.
- A mostly-zeroed theme yields a sensible look via normalisation.
- Widget-specific groups keep scrollbar/slider code free of magic
  numbers.

Negative:

- The struct grows; a host that copies it must use the `size` guard
  rather than `sizeof(lens_theme)` from its own (possibly older) header.
- Normalisation is implicit; a caller that sets `font_size` after
  `lens_create` must call `lens_set_theme` (not mutate in place) to
  re-derive semantic sizes.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0036](0036-lens-input-clipboard-ime.md) — the same `size`-guard
  ABI pattern, applied to input.
