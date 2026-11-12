# reticle — the SVG vector-art document compiler (ADR-0150, ADR-0161).
#
# A separate shared library that links libflux. It owns the SVG static-art
# surface (path grammar, basic-shape decomposition, paint resolution, gradient
# baking, fit-and-paint, headless rasterization) and nothing else: no file IO, no
# text, no images, no scripting, no CSS cascade beyond presentation attributes
# and `style=""`.

reticle_inc = include_directories('include')

reticle_sources = files(
  'src/util.c',
  'src/paint.c',
  'src/path.c',
  'src/parse.c',
  'src/draw.c',
  'src/raster.c',
)

reticle_lib = library('reticle',
  reticle_sources,
  include_directories   : reticle_inc,
  c_args                : ['-DRETICLE_BUILDING=1'],
  dependencies          : [flux_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/reticle/reticle.h',
  'include/reticle/export.h',
  subdir : 'reticle',
)

reticle_dep = declare_dependency(
  link_with           : reticle_lib,
  include_directories : reticle_inc,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries   : reticle_lib,
  name        : 'reticle',
  filebase    : 'reticle',
  description : 'SVG vector-art document compiler: parse once, paint at any size',
  version     : meson.project_version(),
  requires    : ['flux >= ' + meson.project_version()],
)
