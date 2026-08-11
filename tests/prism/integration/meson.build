# Liquid glass material (prism): analytic SDF coverage over a canvas-target
# capture (ADR-0017), and persistent frame-slot output clearing.
test_liquid_glass = executable('test_liquid_glass',
  'test_liquid_glass.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('liquid_glass', test_liquid_glass, suite : 'integration')
