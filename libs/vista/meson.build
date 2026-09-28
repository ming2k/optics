# vista — glTF 2.0 3D scene hierarchy and asset runtime over libflux (ADR-0016, ADR-0106).
#
# A separate shared library that links libflux and feeds the
# flux_scene_draw_mesh primitive. It carries its own pkg-config file so
# consumers link it independently of the rendering engine.

vista_inc = include_directories('include')

vista_sources = files(
  'src/json.c',
  'src/glb.c',
  'src/animation.c',
  'src/scene_graph.c',
)

vista_lib = library('vista',
  vista_sources,
  include_directories : vista_inc,
  c_args              : ['-DVISTA_BUILDING=1'],
  dependencies        : [flux_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion           : optics_soname,
  version             : meson.project_version(),
  install             : true,
)

install_headers('include/vista/vista.h', subdir : 'vista')

vista_dep = declare_dependency(
  link_with           : vista_lib,
  include_directories : vista_inc,
  dependencies        : [flux_dep, m_dep],
)

pkg.generate(
  libraries    : vista_lib,
  name         : 'vista',
  filebase     : 'vista',
  description  : 'glTF 2.0 3D scene hierarchy and asset runtime over flux',
  version      : meson.project_version(),
  requires     : ['flux >= ' + meson.project_version()],
)
