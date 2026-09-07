# Liquid glass material (prism): analytic SDF coverage over a canvas-target
# capture (ADR-0017), and persistent frame-slot output clearing.
test_liquid_glass = executable('test_liquid_glass',
  'test_liquid_glass.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('liquid_glass', test_liquid_glass, suite : 'integration')

# Layered backdrop compositor (prism): frost rects carrying glass bodies in
# one dispatch — the glass samples the frosted layer, not the sharp capture.
test_backdrop_layer = executable('test_backdrop_layer',
  'test_backdrop_layer.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('backdrop_layer', test_backdrop_layer, suite : 'integration')

# Golden regression test (prism liquid glass, default recipe). The
# committed reference lives in tests/prism/golden/; the test reads it via
# PRISM_GOLDEN_SRC so it works from any build dir. Regenerate with
# PRISM_GOLDEN_UPDATE=1 after an *intentional* recipe change — the diff
# in that commit must show the new reference.
test_prism_golden = executable('test_prism_golden',
  'test_prism_golden.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('prism_golden', test_prism_golden,
  suite : 'integration',
  env : ['PRISM_GOLDEN_SRC=' + meson.project_source_root() / 'tests' / 'prism' / 'golden'],
)
