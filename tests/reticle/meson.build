# tests/reticle — the reticle vector compiler's behavioural contract. CPU-only: no Vulkan
# device, no window, no font; everything here runs on flux's software canvas.

test_reticle_parse = executable('test_reticle_parse', 'test_reticle_parse.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_parse', test_reticle_parse, suite : 'reticle')

test_reticle_geometry = executable('test_reticle_geometry', 'test_reticle_geometry.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_geometry', test_reticle_geometry, suite : 'reticle')

test_reticle_paint = executable('test_reticle_paint', 'test_reticle_paint.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_paint', test_reticle_paint, suite : 'reticle')

test_reticle_draw = executable('test_reticle_draw', 'test_reticle_draw.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_draw', test_reticle_draw, suite : 'reticle')

test_reticle_limits = executable('test_reticle_limits', 'test_reticle_limits.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_limits', test_reticle_limits, suite : 'reticle')

test_reticle_leak = executable('test_reticle_leak', 'test_reticle_leak.c',
  dependencies        : [reticle_dep, flux_dep, m_dep],
  include_directories : [reticle_inc, flux_inc],
)
test('reticle_leak', test_reticle_leak, suite : 'reticle')

if get_option('fuzz')
  subdir('fuzz')
endif
