# Reticle — SVG Document Compiler

`reticle` is Optics' device-less SVG static-art compiler. It parses SVG 1.1
vector art into an immutable, refcounted document and paints that document into
a `flux` canvas, a display-list encoder, or a headless CPU pixmap. It owns path
grammar, basic-shape decomposition, paint and gradient resolution, fit-and-paint,
and rasterization. It does not own file IO, text, images, animation, scripting,
or a full CSS cascade.

- Header: `<reticle/reticle.h>`
- Library: `libreticle.so` / `libreticle.dylib` / `reticle.dll`
- pkg-config: `reticle` (declares `Requires: flux`)
- Meson option: `-Dreticle=true` (default; requires `-Dcanvas=true`)
- Rust crate: `reticle` / `reticle-sys`

The compiler is a document-layer sibling of `flux`, not a runtime layer. It
links `flux` and nothing else; consumers that never compile vector art do not
link it.

## Parse Once, Paint Anywhere

```c
#include <reticle/reticle.h>

reticle_document *document = nullptr;
flux_result r = reticle_parse(
    &(reticle_parse_opts){.text = svg_source, .length = RETICLE_LENGTH_MEASURE},
    &document);
if (r != FLUX_OK)
    return r;

/* Paint into a canvas, an encoder, or a headless pixmap. */
r = reticle_draw(&(reticle_draw_opts){
    .document = document,
    .canvas = canvas,
    .bounds = (flux_rect){0, 0, 64, 64},
    .fit = RETICLE_FIT_CONTAIN,
    .paint = RETICLE_PAINT_THEME,
    .color = flux_color_rgba(255, 255, 255, 255),
});
reticle_release(document);
```

`reticle_parse` either returns a complete document or fails with no partial
document. A parsed document is immutable and may be read from multiple threads;
retention is atomic, so the last reference may be released from any thread.

`reticle_parse_opts.length` is always a real byte count. Use
`RETICLE_LENGTH_MEASURE` only for a NUL-terminated buffer whose length is not
already known; there is deliberately no `0 means strlen` shorthand.

## Flat Shape IR

The document exposes a flat, document-ordered shape list with resolved paint:

- `reticle_shape_count` / `reticle_shape_at` enumerate shapes.
- `reticle_viewport` returns the intrinsic size in user units.
- `reticle_gradient_at` resolves a shape's gradient index.
- `reticle_path_emit` appends a shape's geometry to a caller-owned `flux_path`.
- `reticle_attribute` returns a root `data-*` attribute, if present.

A shape carries fill and stroke as values, so a consumer can recolour, mask-bake,
or replay it without re-parsing or walking a scene graph. Group opacity is
multiplied into each descendant shape's `opacity`; groups are not isolated
compositing layers.

## Painting and Rasterization

`reticle_draw` fits the document into a destination box and records into
exactly one of `canvas` or `encoder`. `RETICLE_PAINT_THEME` keeps the document's
literal colours and resolves `currentColor` to `opts.color`;
`RETICLE_PAINT_MONOCHROME` paints every shape in `opts.color` for icon tinting
and mask baking.

`reticle_rasterize` is the headless path. It produces a `reticle_pixmap` in
premultiplied RGBA8, premultiplied BGRA8 (for DRM/KMS cursor planes), or
single-channel coverage. It needs no device, surface, window, or compositor, so
it is safe in CI and on worker threads. Release pixels with
`reticle_pixmap_deinit`.

## Resource Limits

`reticle_parse_limits` bounds shapes, commands, gradients, elements, and nesting
depth. Zero selects the documented default. A ceiling violation fails the parse;
it never returns a truncated document. This is the contract that makes parsing
untrusted SVG safe.

## Example

A complete headless starter is in
[`examples/reticle/rasterize_svg.c`](../../examples/reticle/rasterize_svg.c):

```bash
./build/examples/reticle/rasterize_svg out.ppm
```

## Related Decisions

- [ADR-0150](../adr/0150-svg-vector-document-compiler.md): SVG is a document compiler, not a rendering feature
- [ADR-0161](../adr/0161-reticle-vector-compiler-and-vista-boundary.md): reticle naming and lens parser consolidation
