# tests/reticle — the reticle vector compiler's behavioural contract. CPU-only: no Vulkan
# device, no window, no font; everything here runs on flux's software canvas.

test_reticle_parse = executable('test_reticle_parse', 'test_reticle_parse.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_parse', test_reticle_parse, suite : 'unit')

test_reticle_geometry = executable('test_reticle_geometry', 'test_reticle_geometry.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_geometry', test_reticle_geometry, suite : 'unit')

test_reticle_paint = executable('test_reticle_paint', 'test_reticle_paint.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_paint', test_reticle_paint, suite : 'unit')

test_reticle_draw = executable('test_reticle_draw', 'test_reticle_draw.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_draw', test_reticle_draw, suite : 'integration')

test_reticle_limits = executable('test_reticle_limits', 'test_reticle_limits.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_limits', test_reticle_limits, suite : 'unit')

test_reticle_leak = executable('test_reticle_leak', 'test_reticle_leak.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_leak', test_reticle_leak, suite : 'unit')

if get_option('fuzz')
  subdir('fuzz')
endif
