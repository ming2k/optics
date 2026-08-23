# ADR-0075: System accessibility preferences — lens executes, iris observes, the OS owns

- Status: Accepted
- Date: 2026-08-23
- Scope: lens (text-scale mechanism), iris (preference query/watch on all
  three backends, direct drive into lens), the AT-SPI bridge
  (BoundsChanged, node cap)

## Context

The a11y audit that preceded this ADR found a stack whose accessibility
architecture was right — lens owns a complete semantic tree (ADR-0035),
iris owns the transports (ADR-0043), keyboard navigation and reduced
motion were already implemented — but whose *system-preference* plumbing
was a set of cut wires:

1. `lens_set_reduced_motion` existed and was fully tested, yet nothing in
   iris ever called it. The OS preference a user sets ("minimize
   animation") never reached the one switch designed to receive it.
2. There was no user text scale anywhere. `lens_set_scale` is a
   device-pixel factor (HiDPI); using it for "make text bigger" would
   scale every stroke and every bitmap with the glyphs and would fight
   the compositor's own scale reporting.
3. High contrast was not read, not watched, not applied.
4. The AT-SPI bridge truncated semantic trees at 256 nodes and emitted no
   `BoundsChanged`, so screen-magnifier users lost tracking on any
   non-trivial UI.

None of these can be fixed at the application layer: an app built on
Optics cannot invent a semantic tree, cannot reach the portal/SPI/
NSWorkspace APIs without re-implementing iris, and cannot scale text
without a font-size funnel inside lens. Accessibility in a UI *stack* is
a property of the stack.

## Decision

### 1. lens gains a text-scale factor, orthogonal to the DPI scale

`lens_set_text_scale(ui, factor)` / `lens_text_scale(ui)`, plus a
`lens_desc.text_scale` seed. The factor multiplies **every font-size
token** and nothing else:

- It is folded into `lensi_style_font_size()` — the one funnel every
  widget's measure path and the style resolver already read — so
  measurement, intrinsic widget heights, caret metrics, and paint all see
  the same scaled value with no per-widget work and no clipping.
- Raw semantic size tokens that never pass through a `lens_style`
  (title/heading tokens, the tabs strip, the tooltip) go through the
  `lensi_font_px(ui, token)` helper — the two documented escape hatches.
- Explicit point sizes (`lens_label_ex` family) scale too: an explicit
  size is a design intent ("twice body text"), not an accessibility
  exemption.
- Pure-px geometry (padding, stroke widths, icon glyph boxes) is
  deliberately **not** scaled — text grows, chrome density stays.
- It does not touch `flux_text_set_scale` (raster density). A 1.25 text
  scale at 2x DPI renders 1.25x taller glyphs at the same 2x raster
  crispness. Font sizes already ride the draw-command hash, so a factor
  change invalidates cached canvas records automatically.

### 2. iris observes the OS; a11y is not opt-in

New public header `<iris/a11y_prefs.h>`: `iris_a11y_prefs` (three fields:
`reduced_motion`, `high_contrast`, `text_scale`), a never-failing
`iris_a11y_prefs_query()`, and a live `iris_a11y_prefs_watch()` with the
exact threading contract of `iris_color_scheme_watch` (main-thread
delivery, serialized with the event loop). Sources:

| Platform | reduced motion | high contrast | text scale | change signal |
|---|---|---|---|---|
| Linux | `org.gnome.desktop.interface enable-animations` | `org.freedesktop.appearance contrast` (+ gsettings a11y fallback) | `text-scaling-factor` (+ KDE `fontPackageScale`) | shared portal `SettingChanged` pump |
| Windows | `SPI_GETCLIENTAREAANIMATION` | `SPI_GETHIGHCONTRAST` | `SPI_GETNONCLIENTMETRICS` message-font height vs −12 | `WM_SETTINGCHANGE` |
| macOS | `NSWorkspace accessibilityDisplayShouldReduceMotion` | `accessibilityDisplayIsHighContrastEnabled` | (none global — reports 1.0 honestly) | `NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification` |

Honest absence everywhere: unreadable ⇒ library defaults (false / false /
1.0), which is the correct accessible baseline — never an error, never an
invented value.

All three backends apply the preferences **unconditionally at startup**
and on every change, through their reserved backend watcher slot (the
host's public slot is untouched — same two-slot design as the theme
watcher): `lens_set_reduced_motion`, `lens_set_text_scale`, and a
raised-contrast token mutation for high contrast. Accessibility is not a
host opt-in; a host that wants different policy can watch the same public
API and override after the fact.

### 3. One portal pump, not two

The Linux a11y keys ride the *existing* theme-watcher thread
(`theme_watch_portal.c`): same sd-bus connection, same
`SettingChanged` match, same wakeup-seam delivery. The demux grew a topic
filter; a second watcher with its own connection and poll thread for the
same desktop service was rejected.

### 4. AT-SPI bridge hardening

- `IRIS_A11Y_MAX_NODES` 256 → 1024 (the snapshot arrays are the only cost;
  the walk already short-circuits).
- `visit_fn` now stores each node's solved bounds and the update pass
  emits `BoundsChanged` (right/bottom edges in the two int slots, the
  at-spi2 Component convention) when a surviving node's rect actually
  changed — magnifier tracking survives scrolling and relayout.

## Alternatives considered

- **Scale text via the existing DPI scale.** Rejected: conflates raster
  density with user intent; breaks the documented scale contract
  (`lens.h`, cross-platform.md "Logical pixels everywhere") and the
  compositor's fractional-scale reporting.
- **Apply the factor only at the text seam (measure/draw).** Rejected:
  glyphs would grow inside unscaled boxes and clip — the audit's original
  complaint, just re-created. The funnel placement is the fix.
- **An `iris`-owned font multiplier (no lens API).** Rejected: lens owns
  the font-size tokens; an iris-side multiplier would have to chase every
  entry point and would be invisible to headless lens users (tests,
  offscreen renderers) that also deserve the factor.
- **Per-platform a11y watcher threads.** Rejected on Linux (see §3). On
  Win32 and macOS no thread exists anyway — the OS pushes on the loop
  thread we already own.

## Consequences

- An Optics app now honours the three most impactful OS accessibility
  preferences on every backend with zero host code: reduced motion stops
  eases, text scale grows text and the boxes around it, high contrast
  re-themes to black-on-white with full-strength borders.
- `lensi_style_font_size` gained a `ui` first argument (internal symbol;
  the ~30 call sites updated mechanically). Custom skins that read
  `rs->font_size` keep working untouched — the resolved style already
  carries the factor.
- High contrast is applied as a mutation of the live theme, not a preset
  swap, so a host's customised tokens are overridden *deliberately* and
  restore on toggle-off via the scheme preset.
- What this ADR deliberately does not do: the Win32 UIA and macOS
  NSAccessibility bridges remain stubs (ADR-0056 D5 scheduling still
  applies), `SetCurrentValue`/`SetCaretOffset` write paths remain deferred
  (ADR-0062), and live regions remain unimplemented. The Cocoa a11y-prefs
  file is cross-compiled with an AppKit stub here (same verification
  ceiling as `theme_cocoa.m` — no macOS hardware in CI).

## References

- ADR-0035 (lens semantic tree), ADR-0058 (keyboard modality /
  `focus_visible`), ADR-0062 (bidirectional a11y), ADR-0043/0055 (iris
  foundations, watch APIs and wakeup seam), ADR-0067 (text-input depth —
  the sibling "OS contract reduced to lens fields" precedent)
- `libs/lens/include/lens/lens.h` (`lens_set_text_scale` contract),
  `libs/iris/include/iris/a11y_prefs.h`
- `tests/lens/test_text_scale.c`, `tests/iris/test_a11y_prefs.c`
