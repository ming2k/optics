# Pure region policy for persistent liquid-glass slots: outward/clamped body
# bounds, previous+current clearing, disjoint preservation, and the 64+64 cap.
test_liquid_glass_regions = executable('test_liquid_glass_regions',
  'test_liquid_glass_regions.c',
  dependencies : [material_dep, optics_test_dep, m_dep],
)
test('liquid_glass_regions', test_liquid_glass_regions, suite : 'unit')

# Version surface, capability floor, and null-handle contracts across all five
# material filters without requiring a Vulkan device.
test_material_contracts = executable('test_material_contracts',
  'test_material_contracts.c',
  dependencies : [material_dep, optics_test_dep, m_dep],
)
test('material_contracts', test_material_contracts, suite : 'unit')
