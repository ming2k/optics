# SVG Document Compiler (`optics-svg`)

`optics-svg` (`liboptics_svg`) is Optics' device-less SVG static-art compiler. It parses SVG 1.1
vector art into an immutable, refcounted document and paints that document into
an `optics_gfx_canvas`, a display-list encoder, or a headless CPU pixmap. It owns path
grammar, basic-shape decomposition, paint and gradient resolution, fit-and-paint,
and rasterization. It does not own file IO, text, images, animation, scripting,
or a full CSS cascade.

- Header: `<optics/svg.h>`
- Library: `liboptics_svg.so` / `liboptics_svg.dylib` / `optics_svg.dll`
- pkg-config: `optics-svg` (declares `Requires: optics-gfx`)
- Meson option: `-Dsvg=true` (default)
- Rust crate: `optics-svg` / `optics-svg-sys`

The compiler is a document-layer sibling of `gfx`, not a runtime layer. It
links `liboptics_gfx` and nothing else; consumers that never compile vector art do not
link it.

## Parse Once, Paint Anywhere

```c
#include <optics/svg.h>

optics_svg_document *document = nullptr;
optics_gfx_result r = optics_svg_parse(
    &(optics_svg_parse_opts){.text = svg_source, .length = OPTICS_SVG_LENGTH_MEASURE},
    &document);
if (r != OPTICS_GFX_OK)
    return r;

/* Paint into a canvas, an encoder, or a headless pixmap. */
r = optics_svg_draw(&(optics_svg_draw_opts){
    .document = document,
    .canvas = canvas,
    .bounds = (optics_gfx_rect){0, 0, 64, 64},
    .fit = OPTICS_SVG_FIT_CONTAIN,
    .paint = OPTICS_SVG_PAINT_THEME,
    .color = optics_gfx_color_rgba(255, 255, 255, 255),
});
optics_svg_release(document);
```

`optics_svg_parse` either returns a complete document or fails with no partial
document. A parsed document is immutable and may be read from multiple threads;
retention is atomic, so the last reference may be released from any thread.

`optics_svg_parse_opts.length` is always a real byte count. Use
`OPTICS_SVG_LENGTH_MEASURE` only for a NUL-terminated buffer whose length is not
already known; there is deliberately no `0 means strlen` shorthand.

## Flat Shape IR

The document exposes a flat, document-ordered shape list with resolved paint:

- `optics_svg_shape_count` / `optics_svg_shape_at` enumerate shapes.
- `optics_svg_viewport` returns the intrinsic size in user units.
- `optics_svg_gradient_at` resolves a shape's gradient index.
- `optics_svg_path_emit` appends a shape's geometry to a caller-owned `optics_gfx_path`.
- `optics_svg_attribute` returns a root `data-*` attribute, if present.

A shape carries fill and stroke as values, so a consumer can recolour, mask-bake,
or replay it without re-parsing or walking a scene graph. Group opacity is
multiplied into each descendant shape's `opacity`; groups are not isolated
compositing layers.

## Painting and Rasterization

`optics_svg_draw` fits the document into a destination box and records into
exactly one of `canvas` or `encoder`. `OPTICS_SVG_PAINT_THEME` keeps the document's
literal colours and resolves `currentColor` to `opts.color`;
`OPTICS_SVG_PAINT_MONOCHROME` paints every shape in `opts.color` for icon tinting
and mask baking.

`optics_svg_rasterize` is the headless path. It produces an `optics_svg_pixmap` in
premultiplied RGBA8, premultiplied BGRA8 (for DRM/KMS cursor planes), or
single-channel coverage. It needs no device, surface, window, or compositor, so
it is safe in CI and on worker threads. Release pixels with
`optics_svg_pixmap_deinit`.

## Resource Limits

`optics_svg_parse_limits` bounds shapes, commands, gradients, elements, and nesting
depth. Zero selects the documented default. A ceiling violation fails the parse;
it never returns a truncated document. This is the contract that makes parsing
untrusted SVG safe.

## Example

A complete headless starter is in
[`examples/getting_started/headless_svg.c`](../../examples/getting_started/headless_svg.c):

```bash
./build/examples/getting_started/headless_svg out.ppm
```

## Related Decisions

- [ADR-0150](../adr/0150-svg-vector-document-compiler.md): SVG is a document compiler, not a rendering feature
- [ADR-0161](../adr/0161-reticle-vector-compiler-and-vista-boundary.md): vector compiler and scene boundary
- [ADR-0170](../adr/0170-semantic-module-naming-and-optics-namespace.md): semantic module naming and `optics_` namespace
