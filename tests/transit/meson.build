# tests/transit — property tests for the transit motion vocabulary. CPU-only; no
# Vulkan, no canvas, no lens: the library is pure math.
if get_option('transit')
  test_transit = executable('test_transit', 'test_transit.c',
    dependencies : [transit_dep, m_dep],
    include_directories : transit_inc,
  )
  test('transit', test_transit)
endif
