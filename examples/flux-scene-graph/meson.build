# flux-scene-graph examples — 3D glTF visualizer (ADR-0097).
# Native Iris application.

duck_asset_path = meson.current_source_dir() / 'assets' / 'Duck.glb'

executable('gltf_viewer',
  'gltf_viewer.c',
  dependencies : [flux_sg_dep, iris_dep, m_dep],
  c_args       : ['-DFLUX_DUCK_ASSET="@0@"'.format(duck_asset_path)],
)
