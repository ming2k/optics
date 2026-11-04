# Liquid glass material (prism): analytic SDF coverage over a canvas-target
# capture (ADR-0017), and persistent frame-slot output clearing.
test_liquid_glass = executable('test_liquid_glass',
  'test_liquid_glass.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('liquid_glass', test_liquid_glass, suite : 'integration')

# GPU-bound liquid-glass microbenchmark (ADR-0148). Reports ms/frame, does not
# assert: it exists so a shader change's real cost can be measured instead of
# inferred from static instruction counts. Run directly, never under `meson test`.
bench_liquid_glass_gpu = executable('bench_liquid_glass_gpu',
  'bench_liquid_glass_gpu.c',
  dependencies : [test_helpers_dep, prism_dep],
  build_by_default : false,
)

# Layered backdrop compositor (prism): frost rects carrying glass bodies in
# one dispatch — the glass samples the frosted layer, not the sharp capture.
test_backdrop_layer = executable('test_backdrop_layer',
  'test_backdrop_layer.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('backdrop_layer', test_backdrop_layer, suite : 'integration')

# Mica foundation material (prism): screen-anchored wallpaper composite (ADR-0095).
test_mica = executable('test_mica',
  'test_mica.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('mica', test_mica, suite : 'integration')

# Frosted glass material (prism): dual-Kawase blur sampling, vibrancy boost, tinting.
test_frosted = executable('test_frosted',
  'test_frosted.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('frosted', test_frosted, suite : 'integration')

# Acrylic material (prism): multi-layer composite with luminance plate balancing.
test_acrylic = executable('test_acrylic',
  'test_acrylic.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('acrylic', test_acrylic, suite : 'integration')

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

# Prism material accounting (ADR-0134): each filter has its own ledger row, so
# a material leak is attributed to prism instead of surfacing as a flux-owned
# vk_image. Exact deltas, warm-up iteration, no slack constants.
test_prism_material_leak = executable('test_prism_material_leak',
  'test_prism_material_leak.c',
  dependencies : [test_helpers_dep, prism_dep],
)
test('prism_material_leak', test_prism_material_leak, suite : 'integration')
