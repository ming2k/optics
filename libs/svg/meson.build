# svg — the SVG vector-art document compiler (ADR-0150, ADR-0161).
#
# A separate shared library that links liboptics_gfx. It owns the SVG static-art
# surface (path grammar, basic-shape decomposition, paint resolution, gradient
# baking, fit-and-paint, headless rasterization) and nothing else: no file IO, no
# text, no images, no scripting, no CSS cascade beyond presentation attributes
# and `style=""`.

svg_inc = include_directories('include')

svg_sources = files(
  'src/util.c',
  'src/paint.c',
  'src/path.c',
  'src/parse.c',
  'src/draw.c',
  'src/raster.c',
)

svg_lib = library('optics_svg',
  svg_sources,
  include_directories   : svg_inc,
  c_args                : ['-DOPTICS_SVG_BUILDING=1'],
  dependencies          : [gfx_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/optics/svg.h', subdir : 'optics')
install_headers(
    'include/optics/svg/export.h',
  subdir : 'optics/svg',
)

svg_dep = declare_dependency(
  link_with           : svg_lib,
  include_directories : svg_inc,
  dependencies        : [gfx_dep],
)

pkg.generate(
  libraries   : svg_lib,
  name        : 'optics-svg',
  filebase    : 'optics-svg',
  description : 'SVG vector-art document compiler: parse once, paint at any size',
  version     : meson.project_version(),
  requires    : ['optics-gfx >= ' + meson.project_version()],
)
