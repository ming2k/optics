# tests/svg — the SVG compiler's behavioural contract. CPU-only: no Vulkan
# device, no window, no font; everything here runs on flux's software canvas.
#
# Registered one by one rather than through a `foreach` list: each binary pins a
# different promise, the leak lane names `test_svg_leak` by path, and a gate that
# cannot see a test through a loop would rather report "not registered" than
# guess. Explicit lines keep the mapping from promise to binary visible.

test_svg_parse = executable('test_svg_parse', 'test_svg_parse.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_parse', test_svg_parse)

test_svg_geometry = executable('test_svg_geometry', 'test_svg_geometry.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_geometry', test_svg_geometry)

test_svg_paint = executable('test_svg_paint', 'test_svg_paint.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_paint', test_svg_paint)

test_svg_draw = executable('test_svg_draw', 'test_svg_draw.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_draw', test_svg_draw)

test_svg_limits = executable('test_svg_limits', 'test_svg_limits.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_limits', test_svg_limits)

# The drain assertion for the `svg` resource kind (INV-ARCH-LEDGER). Named by
# tools/config/leak-lanes.toml, so this line is the coverage.
test_svg_leak = executable('test_svg_leak', 'test_svg_leak.c',
  dependencies        : [svg_dep, flux_dep, m_dep],
  include_directories : [svg_inc, flux_inc],
)
test('test_svg_leak', test_svg_leak)

# --- sub-suites ---
# `svg_parse` takes untrusted bytes as its normal input (ADR-0150), which is
# exactly the class of parser the flux-side campaign already fuzzes
# (fuzz_glb_parse, fuzz_path_flatten). The ceiling logic and the tag scanner are
# the highest-risk code in the module, so they get the same lane.
if get_option('fuzz')
  subdir('fuzz')
endif
