# scene — glTF 2.0 3D scene hierarchy and asset runtime over liboptics_gfx (ADR-0016, ADR-0106).
#
# A separate shared library that links liboptics_gfx and feeds the
# optics_gfx_scene_draw_mesh primitive. It carries its own pkg-config file so
# consumers link it independently of the rendering engine.

scene_inc = include_directories('include')

scene_sources = files(
  'src/json.c',
  'src/glb.c',
  'src/animation.c',
  'src/scene_graph.c',
)

scene_lib = library('optics_scene',
  scene_sources,
  include_directories : scene_inc,
  c_args              : ['-DOPTICS_SCENE_BUILDING=1'],
  dependencies        : [gfx_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion           : optics_soname,
  version             : meson.project_version(),
  install             : true,
)

install_headers('include/optics/scene.h', subdir : 'optics')

scene_dep = declare_dependency(
  link_with           : scene_lib,
  include_directories : scene_inc,
  dependencies        : [gfx_dep, m_dep],
)

pkg.generate(
  libraries    : scene_lib,
  name         : 'optics-scene',
  filebase     : 'optics-scene',
  description  : 'glTF 2.0 3D scene hierarchy and asset runtime over gfx',
  version      : meson.project_version(),
  requires     : ['optics-gfx >= ' + meson.project_version()],
)
