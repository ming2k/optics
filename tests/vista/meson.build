# tests/vista — 3D scene hierarchy, TRS node transforms, and glTF asset test suites.
if opt_vista
  test_glb_parse = executable('test_glb_parse',
    'test_glb_parse.c',
    dependencies : [test_helpers_dep, vista_dep],
  )
  test('glb_parse', test_glb_parse, suite : 'vista')

  test_scene_graph_animation = executable('test_scene_graph_animation',
    'test_scene_graph_animation.c',
    dependencies : [test_helpers_dep, vista_dep],
  )
  test('scene_graph_animation', test_scene_graph_animation, suite : 'vista')

  bench_sg_worlds = executable('bench_sg_worlds',
    'bench_sg_worlds.c',
    include_directories : include_directories('../flux/bench'),
    dependencies : [test_helpers_dep, vista_dep, flux_dep],
  )
  test('sg_worlds', bench_sg_worlds, suite : 'bench', is_parallel : false)

  if get_option('fuzz')
    subdir('fuzz')
  endif
endif
