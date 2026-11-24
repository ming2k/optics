# tests/scene — 3D scene hierarchy, TRS node transforms, and glTF asset test suites.
if opt_scene
  test_glb_parse = executable('test_glb_parse',
    'test_glb_parse.c',
    dependencies : [scene_dep, optics_test_dep, m_dep],
  )
  test('glb_parse', test_glb_parse, suite : 'unit')

  test_scene_contracts = executable('test_scene_contracts',
    'test_scene_contracts.c',
    dependencies : [scene_dep, optics_test_dep, m_dep],
  )
  test('scene_contracts', test_scene_contracts, suite : 'unit')

  bench_sg_worlds = executable('bench_sg_worlds',
    'bench_sg_worlds.c',
    include_directories : include_directories('../gfx/bench'),
    dependencies : [optics_gfx_test_support_dep, scene_dep, gfx_dep],
  )
  test('bench_sg_worlds', bench_sg_worlds, suite : 'bench', is_parallel : false)

  subdir('integration')

  if get_option('fuzz')
    subdir('fuzz')
  endif
endif
