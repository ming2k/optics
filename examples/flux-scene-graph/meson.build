# flux-scene-graph examples. Both are windowed (GLFW); skip cleanly when GLFW
# is absent, matching the flux/examples convention.
if not glfw_dep.found()
  warning('GLFW not found; skipping flux-scene-graph examples.')
  subdir_done()
endif

# Bake the source-tree path to the bundled asset into gltf_camera_tour so the
# binary finds Duck.glb regardless of the cwd it is launched from.
duck_asset_path = meson.current_source_dir() / 'assets' / 'Duck.glb'

executable('gltf_viewer',
  'gltf_viewer.c',
  dependencies : [flux_sg_dep, glfw_dep, m_dep],
)

executable('gltf_camera_tour',
  'gltf_camera_tour.c',
  dependencies : [flux_sg_dep, glfw_dep, m_dep],
  c_args       : ['-DFLUX_DUCK_ASSET="@0@"'.format(duck_asset_path)],
)
