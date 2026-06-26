# flux-scene-graph — glTF 2.0 content sibling over libflux (ADR-0016).
#
# A separate shared library that links libflux and feeds the
# flux_scene_draw_mesh primitive. It carries its own pkg-config file so
# consumers link it independently of the rendering engine.

flux_sg_inc = include_directories('include')

flux_sg_sources = files(
  'src/json.c',
  'src/glb.c',
  'src/scene_graph.c',
)

flux_sg_lib = library('flux_scene_graph',
  flux_sg_sources,
  include_directories : flux_sg_inc,
  c_args              : ['-DFLUX_SG_BUILDING=1'],
  dependencies        : [flux_dep],
  gnu_symbol_visibility : 'hidden',
  install             : true,
)

install_headers('include/flux-scene-graph/scene-graph.h', subdir : 'flux-scene-graph')

flux_sg_dep = declare_dependency(
  link_with           : flux_sg_lib,
  include_directories : flux_sg_inc,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries    : flux_sg_lib,
  name         : 'flux-scene-graph',
  filebase     : 'flux-scene-graph',
  description  : 'glTF 2.0 content layer over flux (feeds flux_scene_draw_mesh)',
  version      : meson.project_version(),
  requires     : 'flux',
)


