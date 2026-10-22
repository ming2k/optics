---
id: ADR-0111
title: "Lens Runtime SVG Mixed Paint Runs and Orthogonal Icon Sizing"
status: accepted
date: 2026-10-22
scope: libs/lens, bindings/lens-rs, docs/reference
superseded_by: null
negative_knowledge: true
---

# ADR-0111: Lens Runtime SVG Mixed Paint Runs and Orthogonal Icon Sizing

- Status: Accepted
- Date: 2026-10-22
- Scope: `libs/lens` (`src/icon_svg.c`, `src/skin/selectable.c`, `src/widgets/selectable.c`, `src/style/style.c`, `src/theme/theme.c`, `include/lens/lens.h`), `bindings/lens-rs` (`crates/lens`).
- Amends: [ADR-0076](0076-lens-icon-paint-runs.md).
- Extends: [ADR-0058](0058-lens-widget-state-and-instance-styles.md), [ADR-0061](0061-lens-style-cascade-mechanism-neutral-flavor.md).

## Context

Downstream consumer applications (notably `wavora`'s navigation rail and settings interfaces) uncovered two systemic limitations in `lens`'s icon and list-item presentation subsystems:

1. **Premature collapse of mixed stroke and fill runs in runtime SVGs.**
   [ADR-0076](0076-lens-icon-paint-runs.md) introduced per-shape paint runs (`lens_icon_run`) to preserve distinct fills and strokes. However, ADR-0076's point 3 specified that "an icon whose every run is theme-coloured collapses to `runs == NULL`".
   The implementation in `icon_svg.c` checked only `runs[i].color == 0` without verifying whether all runs shared the same `.fill` mode (`fill` vs `stroke`). Consequently, whenever an icon combined a stroked geometry (e.g. a clock rim or line hands) with a filled geometry (e.g. a center hub) and both used `currentColor`, the parser erroneously discarded the run table (`runs = nullptr`).
   During replay (`replay.c`), whole-icon mode detected `res.has_fill == true`, switching the entire command stream to `LENSI_ICON_RENDER_FILL`. Every stroked path was rendered with zero stroke width, lines vanished entirely, and hollow stroked shapes collapsed into solid blobs ("solid dots").

2. **Rigid coupling of icon size to font size in selectable rows.**
   In `lensi_skin_selectable`, icon dimensions were hardcoded to `rs->font_size`.
   In desktop navigation and list patterns, typography and glyph iconography follow distinct visual weight scales (e.g., 18px–20px iconography paired with 14px–15px text labels). Forcing `icon_size = font_size` constrained complex vector glyphs to a cramped 14px box ($14/24 \approx 0.5833$ scale factor), placing fine geometry on non-integer subpixel boundaries and causing visible aliasing (stair-stepping) and perceived blurring on standard DPI displays. Furthermore, vertical centering without pixel snapping (`(h - icon_size) * 0.5f`) placed glyphs on half-pixel offsets, smearing horizontal strokes across pixel rows.

## Decision

1. **Guard run collapse by uniform fill mode in `icon_svg.c`.**
   Runtime SVG registration collapses theme-colored runs to `runs == nullptr` if and only if every run has identical fill mode (`runs[i].fill == runs[0].fill`). If an icon mixes stroke and fill shapes, `runs` is preserved, allowing `replay.c` to dispatch distinct fill and stroke paths with the active widget color.

2. **Introduce orthogonal `icon_size` style atom and theme token.**
   - Bit 11 of `lens_style_field` is assigned to `LENS_STYLE_ICON_SIZE`.
   - `lens_style` and `lens_style_resolved` acquire `float icon_size`.
   - `lens_theme` acquires `float icon_size` (default `0.0f` = fall back to `font_size`).
   - `lensi_style_resolve` resolves `r.icon_size`: instance override > scope stack > theme icon size > fallback to `r.font_size`. This preserves zero-cost backward compatibility for all existing widgets.

3. **Expose explicit `icon_size` on selectable widgets.**
   - `lens_selectable_opts` acquires `float icon_size` (0 = inherit from resolved style).
   - `lensi_skin_selectable` evaluates `icon_size = rec->content.glyph_size > 0.0f ? rec->content.glyph_size : rs->icon_size`.
   - Geometry positioning in `lensi_skin_selectable` applies `floorf` snapping (`icon_y = floorf((h - icon_size) * 0.5f);`, `text_y = floorf((h - rec->content.text.height) * 0.5f);`, `x = floorf(rs->padding);`), guaranteeing pixel-grid alignment.

4. **Update Rust bindings in `lens-rs`.**
   - `Theme::with_icon_size` and `Theme::icon_size`.
   - `Style::with_icon_size`.
   - `Frame::selectable_icon_sized` alongside standard `Frame::selectable_icon`.

## Consequences

- **Positive:**
  - Runtime SVG assets combining strokes and fills render faithfully with theme colors without geometric collapse.
  - Sidebar and navigation widgets can style icon sizes independently of typography, providing crisp vector presentation.
  - Pixel-grid snapping prevents half-pixel smearing of horizontal and vertical icon segments.
- **Negative Knowledge / Failure Modes:**
  - *Do not assume `color == 0` implies uniform geometry:* Theme color invariance does not equal stroke/fill invariance. Collapsing multi-run streams based on color alone destroys the verb stream's pipeline configuration.
  - *Subpixel icon placement under MSAA:* 4x MSAA is an edge-coverage technique, not a grid hint. Placing small vector strokes (< 1.5px) at fractional coordinates causes phase-shifting blurriness across scanlines.

## Rejected Alternatives

- **Alternative 1: Forcing all runtime icons into pure stroke or pure fill.**
  - *Failure mode:* Artificially restricts icon design expressiveness. Standard SVGs combining a solid center anchor and an outlined outer ring would have to be manually flattened into complex winding polygons, which fail under non-zero winding rules when subpaths are clipped or coincident.
- **Alternative 2: Global text scale enlargement to increase icon size.**
  - *Failure mode:* Upscaling the application-wide font size distorts card, table, and inspector typography hierarchies simply to make navigation icons legible.
- **Alternative 3: Hardcoded icon size offset inside the selectable skin.**
  - *Failure mode:* Violates [ADR-0061](0061-lens-style-cascade-mechanism-neutral-flavor.md) (mechanism vs flavor). Widget skins must remain neutral defaults; visual proportions belong in the style cascade and instance options.
