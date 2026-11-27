# Unit tests: no Vulkan ICD required, no window system. Run on any build host.
# The SPIR-V custom targets below are defined in tests/gfx/meson.build; they
# are inherited through scope, and only device-bound tests consume them.

test_smoke = executable('test_smoke',
  'test_smoke.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('smoke', test_smoke, suite : 'unit')

# Profiler telemetry facade (ADR-0138): zero-overhead macros,
# query API descriptor compliance, and active/passive telemetry states.
test_profile = executable('test_profile',
  'test_profile.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('profile', test_profile, suite : 'unit')

test_math = executable('test_math',
  'test_math.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('math', test_math, suite : 'unit')

# Analytic curve kernel (ADR-0115): reduction error bound, analytic
# inflection/cusp splitting, Wang segment counts, analytic offset, and
# winding under both fill rules. Pure CPU, no device.
test_curve = executable('test_curve',
  'test_curve.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('curve', test_curve, suite : 'unit')

# Analytic distance fields (ADR-0118): sign/magnitude at analytically known
# points, the Eikonal property that licenses "coverage = 0.5 - d", and
# differential parity against the canvas SDF shader's expression.
test_sdf = executable('test_sdf',
  'test_sdf.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('sdf', test_sdf, suite : 'unit')

# Coverage acceptance harness self-verification (ADR-0119 S0). Verifies the
# yardstick, not a rasterizer: area identity, cross-route convergence, the
# self-intersection scoping limit, and the INV-COV-02 level assertion.
test_cover_reference = executable('test_cover_reference',
  'test_cover_reference.c',
  dependencies : [coverage_harness_dep],
)
test('cover_reference', test_cover_reference, suite : 'unit')

# Coverage rasterizer conformance (ADR-0119 S1 gate). The rasterizer is hidden in
# liboptics_gfx.so, so it is compiled into the binary directly — same pattern as
# test_canvas_tess/test_canvas_flatten.
test_coverage_raster = executable('test_coverage_raster',
  'test_coverage_raster.c',
  '../../../libs/gfx/src/canvas/coverage_raster.c',
  '../../../libs/gfx/src/canvas/curve_flatten.c',
  dependencies : [coverage_harness_dep],
)
test('coverage_raster', test_coverage_raster, suite : 'unit')


test_colorspace = executable('test_colorspace',
  'test_colorspace.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('colorspace', test_colorspace, suite : 'unit')

test_icc = executable('test_icc',
  'test_icc.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('icc', test_icc, suite : 'unit')

test_format = executable('test_format',
  'test_format.c',
  dependencies : [gfx_dep, vulkan_dep, m_dep, optics_test_dep],
)
test('format', test_format, suite : 'unit')

test_canvas_paint = executable('test_canvas_paint',
  'test_canvas_paint.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_paint', test_canvas_paint, suite : 'unit')

# Headless software-canvas render test: exercises the CPU backend
# (optics_gfx_canvas_backend_cpu) end to end with no Vulkan device.
test_canvas_cpu = executable('test_canvas_cpu',
  'test_canvas_cpu.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_cpu', test_canvas_cpu, suite : 'unit')

# Supersample factor and coverage-plane contracts (ADR-0112): the knobs the
# ui icon-mask cache is built on. Uses stroked path geometry on purpose —
# rounded rects/circles take the analytic SDF path, whose coverage is
# independent of the sample count and would make this pass vacuously.
test_canvas_coverage = executable('test_canvas_coverage',
  'test_canvas_coverage.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_coverage', test_canvas_coverage, suite : 'unit')

if opt_text
  # Headless CPU-canvas GLYPH run test (ADR-0019): draws text on a device-less
  # canvas via the host R8 coverage atlas path. Links glyph for shaping.
  test_canvas_cpu_text = executable('test_canvas_cpu_text',
    'test_canvas_cpu_text.c',
    dependencies : [optics_gfx_unit_support_dep, text_dep],
  )
  test('canvas_cpu_text', test_canvas_cpu_text, suite : 'unit')

  # Atlas-clear invalidation: a segment recorded before an atlas_clear carries
  # pre-clear glyph UVs, so replay must be refused via the host-atlas
  # generation check (optics_gfx_glyph_run_host_atlas_desc) and a live re-draw must
  # reproduce the recorded pixels exactly. CPU backend; forces real clears
  # with oversized (512px) glyph cells.
  test_canvas_text_atlas = executable('test_canvas_text_atlas',
    'test_canvas_text_atlas.c',
    dependencies : [optics_gfx_unit_support_dep, text_dep],
  )
  test('canvas_text_atlas', test_canvas_text_atlas, suite : 'unit')
endif

# Headless CPU-canvas display-list test: record/replay segments render
# pixel-identically to live emission and refuse stale anchors (clip,
# transform, released handle).
test_canvas_record = executable('test_canvas_record',
  'test_canvas_record.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_record', test_canvas_record, suite : 'unit')

test_canvas_path = executable('test_canvas_path',
  'test_canvas_path.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_path', test_canvas_path, suite : 'unit')

# Structured error slot (ADR-0002 diagnostics): a message composed with
# OPTICS_GFX_FAILF must outlive the producing frame, because optics_gfx_error_info
# borrows it and the caller reads it after the producer returned. result.c
# is compiled in so the internal slot is reachable from a normal link.
test_error_info = executable('test_error_info',
  'test_error_info.c',
  '../../../libs/gfx/src/core/result.c',
  dependencies : [optics_gfx_unit_support_dep],
  c_args : ['-DOPTICS_GFX_STATIC=1'],
)
test('error_info', test_error_info, suite : 'unit')

# curve_flatten.c has hidden visibility in liboptics_gfx.so, so we compile it into
# the test binary directly.
test_canvas_flatten = executable('test_canvas_flatten',
  'test_canvas_flatten.c',
  '../../../libs/gfx/src/canvas/curve_flatten.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_flatten', test_canvas_flatten, suite : 'unit')

# Degenerate-cubic stroke regression, pixel-level through the CPU canvas:
# a zero-length cubic once flooded the flattener scratch and dropped every
# following contour. Links liboptics_gfx normally (public CPU-canvas API).
test_canvas_stroke_degenerate = executable('test_canvas_stroke_degenerate',
  'test_canvas_stroke_degenerate.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_stroke_degenerate', test_canvas_stroke_degenerate, suite : 'unit')

# Stroke outline pieces: joins and caps overlap the bodies without
# double-blending, and crossed inner offsets (bowtie body quads) leave no holes.
test_canvas_stroke_pieces = executable('test_canvas_stroke_pieces',
  'test_canvas_stroke_pieces.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_stroke_pieces', test_canvas_stroke_pieces, suite : 'unit')

# Stroke coverage oracle: a translucent stroke must carry exactly one coverage
# everywhere in its own band. Detects band self-overlap (double blending) that
# is invisible for opaque paint but shows as a darker seam under translucency.
test_canvas_stroke_region = executable('test_canvas_stroke_region',
  'test_canvas_stroke_region.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_stroke_region', test_canvas_stroke_region, suite : 'unit')

# Squircle path <-> field agreement (ADR-0118 INV-SDF-06, ADR-0175): the fitted
# corner cubics must lie on optics_gfx_sdf_rrect's zero set.
test_canvas_squircle = executable('test_canvas_squircle',
  'test_canvas_squircle.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('canvas_squircle', test_canvas_squircle, suite : 'unit')

shape_test = executable('test_canvas_shapes', 'test_canvas_shapes.c', dependencies : [optics_gfx_unit_support_dep])
test('canvas_shapes', shape_test, suite : 'unit')

if opt_text
  test_rfc0094 = executable('test_rfc0094_contracts',
    'test_rfc0094_contracts.c',
    dependencies : [optics_gfx_unit_support_dep, text_dep],
  )
  test('rfc0094_contracts', test_rfc0094, suite : 'unit')
endif

test_display_list = executable('test_display_list', 'test_display_list.c',
  dependencies : [optics_gfx_unit_support_dep],
)
test('display_list', test_display_list, suite : 'unit')

# ADR-0089/0092 RenderPlan IR: target contract, explicit resource versions,
# dependency ordering, writer conflicts, capability subset, and budgets.
# Pure CPU, no device.
test_render_plan = executable('test_render_plan',
  ['test_render_plan.c', '../../../libs/gfx/src/core/render_plan.c'],
  include_directories : [include_directories('../../../libs/gfx/src/core')],
  dependencies : [optics_gfx_unit_support_dep],
)
test('render_plan', test_render_plan, suite : 'unit')

test_public_contracts = executable('test_public_contracts',
  'test_public_contracts.c', dependencies : [optics_gfx_unit_support_dep])
test('public_contracts', test_public_contracts, suite : 'unit')

# ADR-0149: the write-epoch history a swapchain rotation is repaired from.
# Pure policy — no device, no allocation — so every window-arithmetic failure
# mode (an image older than the retained window, "current" vs "never
# synchronised", an intervening full-extent frame) is provable without a GPU.
# The TU is compiled into the test rather than linked: liboptics_gfx builds with
# hidden visibility, and this policy has no public entry point by design.
test_damage_history = executable('test_damage_history',
  ['test_damage_history.c', '../../../libs/gfx/src/core/damage_history.c'],
  include_directories : [include_directories('../../../libs/gfx/src/core')],
  dependencies : [gfx_dep, optics_test_dep],
)
test('damage_history', test_damage_history, suite : 'unit')
