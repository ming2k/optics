---
id: ADR-0150
title: "SVG is a document compiler, not a rendering feature — one library owns the format, every consumer links it"
status: accepted
date: 2026-11-06
superseded_by: null
scope: [svg, flux, lens, architecture, build]
negative_knowledge: true
---

# ADR-0150: SVG is a document compiler, not a rendering feature

- Status: Accepted
- Date: 2026-11-06
- Scope: `libs/svg/`, `meson.build`, `meson_options.txt`, `tests/svg/`,
  `tools/config/leak-lanes.toml`, `tools/guardrails/lockstep.py`,
  `tools/guardrails/symbols.py`
- Amends: [ADR-0063](0063-liquid-glass-material-library.md) (flux keeps the
  mechanism; a document format is not a mechanism)
- Related: [ADR-0136](0136-device-less-by-design.md) (a document is device-less),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md) (the `svg` kind gets a lane),
  [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md) (paths, not
  triangles, are the geometry authority)

---

## Context

SVG arrived in the stack as a *rendering feature*: `libs/lens/src/icon_svg.c` grew
a 1350-line tag scanner so that `lens_icon_register_svg` could accept vector icons,
and it compiled them straight into `lens_icon_cmd` — a 24×24 icon-specific opcode
stream with per-shape colour runs, a fixed viewport normalisation, one stroke
weight, and a `runs == NULL` collapse rule. Five facts about that arrangement are
now wrong for the rest of the stack:

1. **The format is trapped in a UI widget library.** Anything that is not a lens
   icon cannot use it. The consumers that need vectors are elsewhere and growing:
   monochrome icon sets for the shell, the tray, the dock, the launcher, and
   desktop-entry icons; document thumbnails; and (see the companion records) XDG
   cursor themes, where `cursors/<name>.svg` is the format the freedesktop cursor
   specification actually names.

2. **The output is lossy by construction.** Normalising every document into a
   24×24 box with one stroke weight is *correct for icons* and *wrong for
   everything else*: a cursor sprite needs its own 256-unit viewBox and a 10-unit
   stroke, a thumbnail needs the document's real aspect ratio, and a gradient
   needs endpoints in the document's own coordinates. A consumer cannot recover
   what the icon pipeline threw away.

3. **Paint is a run table, not a value.** `lens_icon_run` exists because an icon
   is *recoloured by the widget* after registration. Cursors need the same
   property for a different reason (silhouette vs. theme), and thumbnails need the
   opposite (paint the document faithfully). One representation has to serve all
   three, and it has to be a resolved value on the shape so that recolouring is a
   paint-mode flag rather than a second parser.

4. **A tag scanner inside lens is a layering accident.** Lens is Layer 3. The
   scanner has no device, no UI state, and no widget knowledge; it is a
   *document-format front end* that happens to have been written where it was
   first needed. It is also untested as a format: the lens tests assert icon
   content, not SVG semantics, so every format bug (implicit lineto after
   moveto, arc flag packing, `S` reflection, `objectBoundingBox` units) was
   invisible until an icon happened to look wrong.

5. **The escape hatch is a subprocess.** Where the format *is* needed outside
   lens today, the answer is `fork` + `rsvg-convert`. That is a third-party
   runtime dependency, an unbounded syscall on a render path, and a hard
   requirement that the host ship librsvg — for art the stack could compile
   itself.

## Decision Drivers

- **One owner per format.** A document format must be parsed once, by one
  library, tested against its own specification, and published as a stable IR.
- **Layer purity.** A format front end is not a UI widget, not a renderer, and
  not a material. It must live where it can depend on the rasterization core and
  nothing else.
- **No new device-bound dependencies in flux.** Flux is the device-bound
  rendering foundation; a parser in flux would force every flux consumer to link
  it and would put a tag scanner in the engine that must not contain one.
- **Faithful by default, lossy on request.** The compiler must preserve what the
  document says (coordinates, viewBox, stroke widths, gradient endpoints) and let
  a consumer *request* normalisation, not impose it.
- **Untrusted input is the normal case.** Documents come from theme directories,
  application bundles and downloads. Every ceiling must be explicit, and
  exceeding one must refuse the document rather than truncate it.

## Considered Options

- **Leave the parser in lens and export it.** Rejected: it is not an icon
  concern, its output cannot express a cursor or a thumbnail, and exporting an
  icon-specific opcode stream as a general format API would freeze the wrong IR
  in the ABI forever.

- **Put the parser in flux.** Rejected twice over. Flux is device-bound and
  Layer 0; `libs/svg` is deliberately device-less (ADR-0136) because a parsed
  document must outlive every device. And flux's contract is "irreducible
  primitives" — a document format is a composition of them, not one of them.

- **Keep `rsvg-convert` for the non-icon cases.** Rejected: it makes a raster
  library a runtime dependency of the compositor and the shell, it cannot run on
  the render thread, and it cannot produce a coverage mask or a BGRA cursor
  sprite without a second conversion step.

- **Adopt a third-party SVG crate/library.** Rejected on the same grounds as the
  rest of the dependency policy: the required subset is static vector art, the
  scope is statements about geometry and paint, and the entire surface fits in
  six translation units with its own tests. A dependency would also reintroduce
  the CSS-cascade, font and image-decode trees that the icon pipeline
  deliberately omitted.

- **Support the whole SVG 1.1/2 surface, including `<use>`, `<text>`, `<image>`,
  CSS stylesheets, patterns, masks and filters.** Rejected: those are
  *document-feature* problems with their own dependency trees (fonts, image
  decode, a CSS engine) and none of them can be satisfied by a vector compiler
  that must stay device-less. Their subtrees are skipped, which makes them inert
  rather than wrong, and the boundary is written down below.

## Decision Outcome

### 1. A new Layer-0 sibling library owns the format

`libs/svg` is a shared library that links `libflux` and nothing else. It owns
exactly one domain: turning SVG *static vector art* into flux draw primitives.

```text
libs/svg/src/util.c    version, the libc allocation seam, affine algebra
libs/svg/src/paint.c   numbers, lengths, colours, transforms, gradient resolution
libs/svg/src/path.c    the path grammar and every basic-shape decomposition
libs/svg/src/parse.c   the tag scanner, style cascade, element dispatch, ceilings
libs/svg/src/draw.c    fit → clip → transform → per-shape geometry + brush
libs/svg/src/raster.c  headless pixmap and coverage-mask readback
```

Public surface (`<svg/svg.h>`): `svg_parse` / `svg_retain` / `svg_release` /
`svg_live_documents`, `svg_viewport`, `svg_attribute`, `svg_shape_count` /
`svg_shape_at`, `svg_gradient_at`, `svg_path_emit`, `svg_draw`, `svg_rasterize`,
`svg_pixmap_deinit`.

### 2. The document is an immutable, refcounted, flat shape list

Parsing is separated from painting, and the parse result is immutable:
`svg_draw` and `svg_rasterize` may run any number of times at any size, from any
thread, without reparsing. Refcounting is atomic so a document can be shared and
released from whichever thread holds the last reference.

The IR is a **flat, document-ordered array of painted shapes**, each owning:

- its path as the five verbs `MOVE / LINE / QUAD / CUBIC / CLOSE`, already
  transformed into document user space (arcs, ellipses, rounded rects and smooth
  reflections are decomposed at parse time, so no consumer re-implements SVG
  geometry);
- resolved `fill` and `stroke` as `svg_paint` values — `NONE`,
  `CURRENT_COLOR`, `SOLID`, or a gradient index — **plus** a separate
  `opacity`, so a consumer that recolours keeps the shape's own alpha;
- stroke topology (`width`, `miter_limit`, `cap`, `join`), `fill_rule`, the
  cascaded element `opacity`, and exact geometric bounds (stroke excluded, cubic
  extrema included).

A flat list with paint-as-value is the shape the consumers need, and it is why
the three use cases are paint-mode flags rather than three code paths:
`SVG_PAINT_THEME` paints the document faithfully, `SVG_PAINT_MONOCHROME` replaces
every painted shape's colour with the caller's while keeping coverage and
opacity.

### 3. Faithful by default, normalisation on request

Coordinates stay in the document's own user space. `svg_draw` takes a destination
box and a fit (`CONTAIN` uniform-and-centred, or `STRETCH`), clips to that box,
and applies the fit transform; `svg_path_emit` exposes the same geometry for a
caller that wants to drive the transform itself. `svg_viewport` reports the
document's intrinsic size — `viewBox`, else `width`/`height`, else the geometric
union of its shapes — and never zero.

`data-*` attributes on the root `<svg>` are preserved verbatim and read back with
`svg_attribute`. SVG has no vocabulary for application metadata, and carriers
(cursor themes, icon sets) stamp it there; the compiler does not invent semantics
of its own and does not drop what it does not understand.

### 4. Device-less by design, with its own drain counter

A document is allocated from libc and released only by `svg_release`
(ADR-0136). It cannot borrow a device allocator without making document lifetime
depend on device creation order, which would be a worse defect than the one
attribution solves. `svg_live_documents` is the drain counter, and
`tests/svg/test_svg_leak.c` is the lane that asserts **exact** deltas across
success, failure, retention, rasterization and a hundred parse/release cycles.

### 5. Every ceiling is explicit, and exceeding one is a refusal

`svg_parse_limits` bounds shapes, path commands, gradients, elements and nesting
depth, and total input bytes is capped independently. A document that exceeds any
of them fails to parse. It is never truncated.

Structural balance is part of the contract: a buffer that ends mid-subtree, or
inside a comment, is refused. An unterminated document is precisely the
"truncated document" this library promises never to produce, and reporting
success would paint a fraction of the art with no indication why.

### 6. The format boundary is stated, not implied

**Supported:** `<svg>`, `<g>`, `<path>`, `<rect>` (incl. `rx`/`ry`),
`<circle>`, `<ellipse>`, `<line>`, `<polyline>`, `<polygon>`,
`<linearGradient>`, `<radialGradient>`, `<stop>`, `<defs>` as a definition
container; presentation attributes and `style=""` declarations for
`fill`, `stroke`, `color`, `stroke-width`, `fill-opacity`, `stroke-opacity`,
`opacity`, `fill-rule`, `stroke-linecap`, `stroke-linejoin`,
`stroke-miterlimit`; `transform` (`matrix`, `translate`, `scale`, `rotate`,
`skewX`, `skewY`, composed left to right); CSS colour keywords, `#rgb`,
`#rrggbb`, `#rrggbbaa`, `rgb()`, `transparent`; gradient `href`/`xlink:href`
inheritance, `gradientUnits` (`objectBoundingBox` and `userSpaceOnUse`, with the
bounding-box mapping baked per shape), `gradientTransform`, `<stop>` offsets and
`stop-opacity`.

**Skipped, and inert rather than wrong** — their subtrees are entered in skip
mode, so a `<mask>`'s black rect cannot paint over the art it was meant to mask
and a `<style>` sheet's markup cannot surface as shapes:
`<use>`, `<symbol>`, `<text>`, `<image>`, `<clipPath>`, `<mask>`, `<pattern>`,
`<marker>`, `<filter>`, `<style>`, `<script>`, `<title>`, `<desc>`,
`<metadata>`.

**Group opacity** multiplies into each descendant shape rather than compositing
an isolated layer. Isolated compositing needs a tree-shaped IR or a saved layer
per group, and neither survives the flat per-shape recolour that the mask bake
and the cursor silhouette depend on. Art that requires overlapping children to
composite as a group must be pre-flattened. Every other opacity —
element, fill, stroke, stop — is exact.

**Gradient spread** is always pad: flux gradients do not tile, and a
`spreadMethod` of `reflect`/`repeat` is recorded but not honoured. A
single-stop gradient becomes a flat colour (SVG's own meaning); a zero-length
linear gradient paints its last stop; a gradient with no reachable stops paints
nothing.

### 7. Consumers link the library; the icon stream becomes an adapter

`lens_icon_register_svg` keeps its signature and its process-global id
arithmetic, and its body becomes an adapter over `svg_parse` — the icon opcode
stream is a *projection* of a document, not a second parser. The subprocess
escape hatches (`rsvg-convert` for application icons and tray icons) are removed
in favour of the same library, at which point the only runtime SVG handling in
the stack is this one, in-process, device-less, and tested.

## Invariants & Behavioral Boundaries

- `[INV-SVG-01]` **One parser.** No other component in the tree may parse SVG
  markup. A consumer reads a `svg_document`; a component that needs a different
  output shape projects from the document.
- `[INV-SVG-02]` **Parse and paint are separate.** A document is immutable after
  `svg_parse`; `svg_draw`/`svg_rasterize` may be called any number of times, at
  any size, from any thread, and must not mutate it.
- `[INV-SVG-03]` **No truncated document.** Exceeding a ceiling, ending the input
  mid-subtree, or ending it inside a comment fails the parse and returns no
  document.
- `[INV-SVG-04]` **Paint is a value on the shape.** Recolouring is a paint mode,
  never a reparse. `CURRENT_COLOR` survives parsing as its own paint kind unless
  an ancestor declared `color`.
- `[INV-SVG-05]` **Device-less.** A document outlives every flux device and is
  released only by `svg_release`; `svg_live_documents` must return to its baseline
  across every path in `tests/svg/test_svg_leak.c`.
- `[INV-SVG-06]` **A disabled element is not a shape.** A zero extent, a zero
  radius, an empty `d`, or an unparseable geometry attribute yields *no shape* —
  never an empty shape, because shape indices are a consumer-visible contract.
- `[INV-SVG-07]` **A viewport is never zero.** An empty document reports 1×1 so a
  consumer can divide by it unconditionally.
- `[INV-SVG-08]` **Element opacity reaches the pixels.** A shape's cascaded
  `opacity` multiplies into its paints; parsing it and dropping it on the floor is
  the same defect as not parsing it, and only a rasterized assertion catches it.
- `[INV-SVG-09]` **A stroke width is in user units and scales exactly once.** The
  canvas transform already scales it; a painter must hand flux the raw width, and
  an explicit override expressed in destination units must be divided by the fit
  scale for that transform to undo.

## Rejected Alternatives & Negative Knowledge

- **Never put a document format inside a device-bound or UI-layer library.** The
  format outlives both, and putting it there makes it impossible to test against
  its own specification instead of against one consumer's expectations.

- **Never normalise a document at parse time.** Fixed viewports, single stroke
  weights and collapsed paint tables are consumer policy. A parser that applies
  them destroys information no consumer can recover.

- **Never let a parse failure produce a partial document.** Half an icon renders
  plausibly; a refusal does not.

- **Never rely on a subprocess for a format the stack can compile itself.** A
  `fork` on a render path is an unbounded syscall, a third-party runtime
  dependency, and an untestable seam.

- **Never apply a per-shape paint to the "not painted" half.** Substituting the
  theme colour for `SVG_PAINT_NONE` in monochrome mode gives every fill-only shape
  a full-weight stroke in its own colour, which covers the art it was meant to
  paint.

## Two audit findings this record originally got wrong

Both were found by probing the built library rather than by reading it, and both
are now pinned by `tests/svg/test_svg_draw.c`. They are recorded here because the
lesson is about *what the first version's tests could see*, not about the code:

1. **`opacity` was parsed, stored, and never applied.** `svg_shape_view::opacity`
   existed and was correct; `svg_draw` used the caller's global alpha for every
   shape and never multiplied the shape's own. A test asserting
   `shape.opacity() == 0.25` passed while the picture painted at 100%. The
   assertion that matters is on the rasterized alpha.
2. **Stroke width was scaled twice.** The document's width was multiplied by the
   fit scale *and* handed to a canvas whose transform also scales it, so a 1-unit
   stroke came out at `scale²` device pixels — 4 px at 8 px output but 16 px at
   32 px output. Every icon rendered at a scale other than 1.0 had fattened
   outlines, and the 1.0 case looked perfect.

The generalisation is INV-SVG-08/09 above: a per-shape property that is stored
but not consumed, and a unit that is transformed by two layers, are both
invisible to an API-shape test and both obvious to a pixel assertion.

## Consequences

### Positive

- One tested implementation of the format, with the specification's awkward
  corners (implicit lineto after moveto, arc flag packing, `S`/`T` reflection,
  comma-or-space path separators, `objectBoundingBox` units) each pinned by a
  named check.
- Cursors, icons, thumbnails and any future vector consumer share one parser,
  one paint model and one set of ceilings.
- The subprocess dependency on `rsvg-convert` can be deleted, and with it the
  requirement that a host ship librsvg for the stack's own chrome.
- A leak lane, a lockstep header entry and a symbols entry make the new library
  subject to the same mechanical gates as every other one.

### Negative / Trade-offs

- A new shared library to build, install and version.
- Group opacity is a documented boundary, not a feature; art that needs isolated
  group compositing must be flattened first.
- `<use>`, `<text>`, masks, patterns and CSS stylesheets are inert. A theme that
  relies on them loses that content rather than approximating it.
- The tag scanner is not an XML processor: it does not expand entities, does not
  resolve namespaces, and tolerates markup a strict parser would reject. That is
  deliberate (it removes the entity-expansion and namespace-quadratic attack
  classes) and it is the documented cost of having no XML dependency.

## References

- The freedesktop cursor specification (`cursors/<name>.svg`, `Inherit=`) is the
  consumer that motivated this record; see the companion decision on XDG cursor
  themes.
- [ADR-0136](0136-device-less-by-design.md), [ADR-0130](0130-leak-coverage-mechanical-gate.md),
  [ADR-0063](0063-liquid-glass-material-library.md),
  [ADR-0115](0115-analytic-curve-kernel-single-geometry-authority.md).
- SVG 1.1 implementation notes F.6.5/F.6.6 (endpoint → centre arc
  parameterisation), which `libs/svg/src/path.c` implements.
