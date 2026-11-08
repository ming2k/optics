# svg — the SVG vector-document compiler (ADR-0150).
#
# A separate shared library that links libflux. It owns the SVG static-art
# surface (path grammar, basic-shape decomposition, paint resolution, gradient
# baking, fit-and-paint, headless rasterization) and nothing else: no file IO, no
# text, no images, no scripting, no CSS cascade beyond presentation attributes
# and `style=""`.
#
# Why a sibling and not part of flux: flux is the device-bound rendering
# foundation (ADR-0063 Layer 0), and SVG is a *document format* — a parser plus a
# compiler onto flux's primitives. Putting it in flux would put a tag scanner in
# the engine that must not contain one, and would make every flux consumer link
# it. Lens, iris, prism and any application that needs vectors link it instead.

svg_inc = include_directories('include')

svg_sources = files(
  'src/util.c',
  'src/paint.c',
  'src/path.c',
  'src/parse.c',
  'src/draw.c',
  'src/raster.c',
)

svg_lib = library('svg',
  svg_sources,
  include_directories   : svg_inc,
  c_args                : ['-DSVG_BUILDING=1'],
  dependencies          : [flux_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/svg/svg.h',
  'include/svg/export.h',
  subdir : 'svg',
)

svg_dep = declare_dependency(
  link_with           : svg_lib,
  include_directories : svg_inc,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries   : svg_lib,
  name        : 'svg',
  filebase    : 'svg',
  description : 'SVG vector-document compiler: parse once, paint at any size',
  version     : meson.project_version(),
  requires    : ['flux >= ' + meson.project_version()],
)
