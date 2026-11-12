# Unit tests: no Vulkan ICD required, no window system. Run on any
# build host. The custom_target SPIR-V binaries used only by
# integration tests are defined in tests/meson.build (parent) and
# pulled in here as plain variables (subdir() shares scope).

test_smoke = executable('test_smoke',
  'test_smoke.c',
  dependencies : [test_helpers_dep],
)
test('smoke', test_smoke, suite : 'unit')

# Observation-plane ledger (ADR-0129): unconditional counting, peak
# surviving drain, unmatched-release rejection, bounded interning that
# reports its own truncation, and the report's structured return shape.
test_observe = executable('test_observe',
  'test_observe.c',
  dependencies : [test_helpers_dep],
)
test('observe', test_observe, suite : 'unit')

# Profiler telemetry facade (ADR-0138): zero-overhead macros,
# query API descriptor compliance, and active/passive telemetry states.
test_profile = executable('test_profile',
  'test_profile.c',
  dependencies : [test_helpers_dep],
)
test('profile', test_profile, suite : 'unit')

# One versioning scheme across every library: macros round-trip through
# the runtime accessors, packed numbers agree, and the version_string()
# output is derived from the macros (a hard-coded literal like the
# scene-graph "0.0.29" incident fails here). CPU-only.
test_version_lockstep = executable('test_version_lockstep',
  'test_version_lockstep.c',
  dependencies : [test_helpers_dep, transit_dep, prism_dep, iris_dep,
                  vista_dep, lens_dep, glyph_dep],
)
test('version_lockstep', test_version_lockstep, suite : 'unit')

test_math = executable('test_math',
  'test_math.c',
  dependencies : [test_helpers_dep],
)
test('math', test_math, suite : 'unit')

# Analytic curve kernel (ADR-0115): reduction error bound, analytic
# inflection/cusp splitting, Wang segment counts, analytic offset, and
# winding under both fill rules. Pure CPU, no device.
test_curve = executable('test_curve',
  'test_curve.c',
  dependencies : [test_helpers_dep],
)
test('curve', test_curve, suite : 'unit')

# Analytic distance fields (ADR-0118): sign/magnitude at analytically known
# points, the Eikonal property that licenses "coverage = 0.5 - d", and
# differential parity against the canvas SDF shader's expression.
test_sdf = executable('test_sdf',
  'test_sdf.c',
  dependencies : [test_helpers_dep],
)
test('sdf', test_sdf, suite : 'unit')

# Algorithm Vault (ADR-0121): pure ear-clipping tessellation asset tests
test_vault_tess = executable('test_vault_tess',
  'test_vault_tess.c',
  dependencies : [test_helpers_dep],
)
test('vault_tess', test_vault_tess, suite : 'unit')

# Coverage acceptance harness self-verification (ADR-0119 S0). Verifies the
# yardstick, not a rasterizer: area identity, cross-route convergence, the
# self-intersection scoping limit, and the INV-COV-02 level assertion.
test_cover_reference = executable('test_cover_reference',
  'test_cover_reference.c',
  dependencies : [coverage_harness_dep],
)
test('cover_reference', test_cover_reference, suite : 'unit')

# Coverage rasterizer conformance (ADR-0119 S1 gate). The rasterizer is hidden in
# libflux.so, so it is compiled into the binary directly — same pattern as
# test_canvas_tess/test_canvas_flatten.
test_coverage_raster = executable('test_coverage_raster',
  'test_coverage_raster.c',
  '../../../libs/flux/src/canvas/coverage_raster.c',
  '../../../libs/flux/src/canvas/curve_flatten.c',
  dependencies : [coverage_harness_dep],
)
test('coverage_raster', test_coverage_raster, suite : 'unit')


test_colorspace = executable('test_colorspace',
  'test_colorspace.c',
  dependencies : [test_helpers_dep],
)
test('colorspace', test_colorspace, suite : 'unit')

test_icc = executable('test_icc',
  'test_icc.c',
  dependencies : [test_helpers_dep],
)
test('icc', test_icc, suite : 'unit')

test_format = executable('test_format',
  'test_format.c',
  dependencies : [test_helpers_dep],
)
test('format', test_format, suite : 'unit')

test_canvas_paint = executable('test_canvas_paint',
  'test_canvas_paint.c',
  dependencies : [test_helpers_dep],
)
test('canvas_paint', test_canvas_paint, suite : 'unit')

# Headless software-canvas render test: exercises the CPU backend
# (flux_canvas_backend_cpu) end to end with no Vulkan device.
test_canvas_cpu = executable('test_canvas_cpu',
  'test_canvas_cpu.c',
  dependencies : [test_helpers_dep],
)
test('canvas_cpu', test_canvas_cpu, suite : 'unit')

# Supersample factor and coverage-plane contracts (ADR-0112): the knobs the
# lens icon-mask cache is built on. Uses stroked path geometry on purpose —
# rounded rects/circles take the analytic SDF path, whose coverage is
# independent of the sample count and would make this pass vacuously.
test_canvas_coverage = executable('test_canvas_coverage',
  'test_canvas_coverage.c',
  dependencies : [test_helpers_dep],
)
test('canvas_coverage', test_canvas_coverage, suite : 'unit')

# Headless CPU-canvas GLYPH run test (ADR-0019): draws text on a device-less
# canvas via the host R8 coverage atlas path. Links glyph for shaping.
test_canvas_cpu_text = executable('test_canvas_cpu_text',
  'test_canvas_cpu_text.c',
  dependencies : [test_helpers_dep, glyph_dep],
)
test('canvas_cpu_text', test_canvas_cpu_text, suite : 'unit')

# Long-session text soak: drives the glyph cache past its live ceiling and
# replays resident working sets, asserting the diagnostic counters (evictions,
# hits vs misses, atlas pages/clears) stay in their documented steady states.
# The soak's 9000-glyph working set needs a real CJK face; CI only guarantees
# one on Linux (fonts-noto-cjk), while macOS fallback faces are not loadable
# by FreeType and Windows runner fonts vary.
if host_machine.system() == 'linux'
  test_text_soak = executable('test_text_soak',
    'test_text_soak.c',
    dependencies : [test_helpers_dep, glyph_dep],
  )
  test('text_soak', test_text_soak, suite : 'unit', timeout : 120)
endif

# Atlas-clear invalidation: a segment recorded before an atlas_clear carries
# pre-clear glyph UVs, so replay must be refused via the host-atlas
# generation check (flux_glyph_run_host_atlas_desc) and a live re-draw must
# reproduce the recorded pixels exactly. CPU backend; forces real clears
# with oversized (512px) glyph cells.
test_canvas_text_atlas = executable('test_canvas_text_atlas',
  'test_canvas_text_atlas.c',
  dependencies : [test_helpers_dep, glyph_dep],
)
test('canvas_text_atlas', test_canvas_text_atlas, suite : 'unit')

# Headless CPU-canvas display-list test: record/replay segments render
# pixel-identically to live emission and refuse stale anchors (clip,
# transform, released handle).
test_canvas_record = executable('test_canvas_record',
  'test_canvas_record.c',
  dependencies : [test_helpers_dep],
)
test('canvas_record', test_canvas_record, suite : 'unit')

test_canvas_path = executable('test_canvas_path',
  'test_canvas_path.c',
  dependencies : [test_helpers_dep],
)
test('canvas_path', test_canvas_path, suite : 'unit')

# Structured error slot (ADR-0002 diagnostics): a message composed with
# FLUX_FAILF must outlive the producing frame, because flux_error_info
# borrows it and the caller reads it after the producer returned. result.c
# is compiled in so the internal slot is reachable from a normal link.
test_error_info = executable('test_error_info',
  'test_error_info.c',
  '../../../libs/flux/src/core/result.c',
  dependencies : [test_helpers_dep],
  c_args : ['-DFLUX_STATIC=1'],
)
test('error_info', test_error_info, suite : 'unit')

# The tessellator test compiles geometry_tess.c in directly so it
# can call signed_area / ear_clip_contour — those symbols have
# hidden visibility in libflux.so and aren't reachable from a
# normal link.
test_canvas_tess = executable('test_canvas_tess',
  'test_canvas_tess.c',
  '../../../libs/flux/src/canvas/geometry_tess.c',
  dependencies : [test_helpers_dep],
)
test('canvas_tess', test_canvas_tess, suite : 'unit')

# Tessellation result cache: CPU-canvas pixel comparisons plus the
# hit/miss/store counters read from struct flux_canvas via the
# internal header. Links libflux normally (no source compiled in).
test_canvas_tess_cache = executable('test_canvas_tess_cache',
  'test_canvas_tess_cache.c',
  dependencies : [test_helpers_dep],
)
test('canvas_tess_cache', test_canvas_tess_cache, suite : 'unit')

# Same reasoning as test_canvas_tess: curve_flatten.c is hidden
# in libflux.so, so we compile it into the test binary directly.
test_canvas_flatten = executable('test_canvas_flatten',
  'test_canvas_flatten.c',
  '../../../libs/flux/src/canvas/curve_flatten.c',
  dependencies : [test_helpers_dep],
)
test('canvas_flatten', test_canvas_flatten, suite : 'unit')

# Degenerate-cubic stroke regression, pixel-level through the CPU canvas:
# a zero-length cubic once flooded the flattener scratch and dropped every
# following contour. Links libflux normally (public CPU-canvas API).
test_canvas_stroke_degenerate = executable('test_canvas_stroke_degenerate',
  'test_canvas_stroke_degenerate.c',
  dependencies : [test_helpers_dep],
)
test('canvas_stroke_degenerate', test_canvas_stroke_degenerate, suite : 'unit')

# Stroke coverage oracle: a translucent stroke must carry exactly one coverage
# everywhere in its own band. Detects band self-overlap (double blending) that
# is invisible for opaque paint but shows as a darker seam under translucency.
test_canvas_stroke_region = executable('test_canvas_stroke_region',
  'test_canvas_stroke_region.c',
  dependencies : [test_helpers_dep],
)
test('canvas_stroke_region', test_canvas_stroke_region, suite : 'unit')

# Squircle path <-> field agreement (ADR-0118 INV-SDF-06): the fitted corner
# cubics must lie on flux_sdf_squircle's zero set, so the canvas and the Prism
# materials describe one shape rather than two.
test_canvas_squircle = executable('test_canvas_squircle',
  'test_canvas_squircle.c',
  dependencies : [test_helpers_dep],
)
test('canvas_squircle', test_canvas_squircle, suite : 'unit')

test_scene_camera = executable('test_scene_camera',
  'test_scene_camera.c',
  dependencies : [test_helpers_dep],
)
test('scene_camera', test_scene_camera, suite : 'unit')

# glyph_cache compiles its source files directly (matching the
# test_canvas_tess pattern). The cache is a pure data structure with no
# Vulkan / Fontconfig dependencies, so the cache half runs on any build
# host and gates the "candidates lag after a while" regression described
# in CHANGELOG-TEXT.md. The atlas half drives the shelf packer's page
# state machine, which needs the FreeType headers text_internal.h
# includes (no FreeType calls execute on these paths).
# The cache TU now carries allocation attribution, so it links flux for the
# allocator entry points even though the test only exercises the hash table
# (ADR-0135).
test_glyph_cache = executable('test_glyph_cache',
  'test_glyph_cache.c',
  dependencies : [flux_dep],
)
test('glyph_cache', test_glyph_cache, suite : 'unit')

shape_test = executable('test_canvas_shapes', 'test_canvas_shapes.c', dependencies : [test_helpers_dep])
test('canvas_shapes', shape_test, suite : 'unit')

test_rfc0094 = executable('test_rfc0094_contracts',
  'test_rfc0094_contracts.c',
  dependencies : [test_helpers_dep, glyph_dep],
)
test('rfc0094_contracts', test_rfc0094, suite : 'unit')

test_display_list = executable('test_display_list', 'test_display_list.c',
  dependencies : [test_helpers_dep],
)
test('display_list', test_display_list, suite : 'unit')

# ADR-0089/0092 RenderPlan IR: target contract, explicit resource versions,
# dependency ordering, writer conflicts, capability subset, and budgets.
# Pure CPU, no device.
test_render_plan = executable('test_render_plan',
  ['test_render_plan.c', '../../../libs/flux/src/core/render_plan.c'],
  include_directories : [include_directories('../../../libs/flux/src/core')],
  dependencies : [test_helpers_dep],
)
test('render_plan', test_render_plan, suite : 'unit')

test_public_contracts = executable('test_public_contracts',
  'test_public_contracts.c', dependencies : [test_helpers_dep])
test('public_contracts', test_public_contracts, suite : 'unit')

# ADR-0149: the write-epoch history a swapchain rotation is repaired from.
# Pure policy — no device, no allocation — so every window-arithmetic failure
# mode (an image older than the retained window, "current" vs "never
# synchronised", an intervening full-extent frame) is provable without a GPU.
# The TU is compiled into the test rather than linked: libflux builds with
# hidden visibility, and this policy has no public entry point by design.
test_damage_history = executable('test_damage_history',
  ['test_damage_history.c', '../../../libs/flux/src/core/damage_history.c'],
  include_directories : [include_directories('../../../libs/flux/src/core')],
  dependencies : [flux_dep],
)
test('damage_history', test_damage_history, suite : 'unit')
