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
