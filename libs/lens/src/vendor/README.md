# Vendored third-party sources

## nanosvg

- **Origin:** https://github.com/memononen/nanosvg
- **Version:** master @ `239e102ec2c691f2902e20ace2ed36ee4a35cfe6` (2026-07-09)
- **License:** zlib (see the license header at the top of `nanosvg.h` — kept
  intact, as required).

Files:

- `nanosvg.h` — the SVG parser, unmodified upstream header. Used by
  `../icon_runtime.c` to parse application-supplied SVG icon assets for
  `lens_icon_register_svg`.
- `nanosvg.c` — the single implementation translation unit
  (`NANOSVG_IMPLEMENTATION`).

`nanosvgrast.h` (the rasterizer) is deliberately **not** vendored: lens only
needs the path geometry — tessellation and painting go through flux's canvas,
so icons stay resolution-independent and theme-tinted.

To update: replace `nanosvg.h` with the newer upstream copy, refresh the
commit hash above, and re-run the lens test suite (`meson test -C build`).
