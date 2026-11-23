# tests/svg — the svg vector compiler's behavioural contract. CPU-only: no Vulkan
# device, no window, no font; everything here runs on gfx's software canvas.

test_svg_parse = executable('test_svg_parse', 'test_svg_parse.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_parse', test_svg_parse, suite : 'unit')

test_svg_geometry = executable('test_svg_geometry', 'test_svg_geometry.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_geometry', test_svg_geometry, suite : 'unit')

test_svg_paint = executable('test_svg_paint', 'test_svg_paint.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_paint', test_svg_paint, suite : 'unit')

test_svg_draw = executable('test_svg_draw', 'test_svg_draw.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_draw', test_svg_draw, suite : 'integration')

test_svg_limits = executable('test_svg_limits', 'test_svg_limits.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_limits', test_svg_limits, suite : 'unit')

test_svg_leak = executable('test_svg_leak', 'test_svg_leak.c',
  dependencies        : [svg_dep, gfx_dep, m_dep, optics_test_dep],
  include_directories : [svg_inc, gfx_inc],
)
test('svg_leak', test_svg_leak, suite : 'unit')

if get_option('fuzz')
  subdir('fuzz')
endif
