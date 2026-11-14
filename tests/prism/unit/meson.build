# Pure region policy for persistent liquid-glass slots: outward/clamped body
# bounds, previous+current clearing, disjoint preservation, and the 64+64 cap.
test_liquid_glass_regions = executable('test_liquid_glass_regions',
  'test_liquid_glass_regions.c',
  dependencies : [prism_dep, optics_test_dep, m_dep],
)
test('liquid_glass_regions', test_liquid_glass_regions, suite : 'unit')
