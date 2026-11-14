# tests/vista — 3D scene hierarchy, TRS node transforms, and glTF asset test suites.
if opt_vista
  test_glb_parse = executable('test_glb_parse',
    'test_glb_parse.c',
    dependencies : [vista_dep, optics_test_dep, m_dep],
  )
  test('glb_parse', test_glb_parse, suite : 'unit')

  bench_sg_worlds = executable('bench_sg_worlds',
    'bench_sg_worlds.c',
    include_directories : include_directories('../flux/bench'),
    dependencies : [flux_test_support_dep, vista_dep, flux_dep],
  )
  test('bench_sg_worlds', bench_sg_worlds, suite : 'bench', is_parallel : false)

  subdir('integration')

  if get_option('fuzz')
    subdir('fuzz')
  endif
endif
