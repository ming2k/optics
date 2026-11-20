# tests/transit — property tests for the transit motion vocabulary. CPU-only; no
# Vulkan, no canvas, no lens: the library is pure math.
if get_option('motion')
  test_transit = executable('test_transit', 'test_transit.c',
    dependencies : [motion_dep, m_dep, optics_test_dep],
    include_directories : motion_inc,
  )
  test('motion', test_transit, suite : 'unit')
endif
