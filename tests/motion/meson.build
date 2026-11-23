# tests/motion — property tests for the motion vocabulary. CPU-only; no
# Vulkan, no canvas, no ui: the library is pure math.
if get_option('motion')
  test_motion = executable('test_motion', 'test_motion.c',
    dependencies : [motion_dep, m_dep, optics_test_dep],
    include_directories : motion_inc,
  )
  test('motion', test_motion, suite : 'unit')
endif
