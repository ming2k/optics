# Unit tests: no Vulkan ICD required, no window system. Run on any
# build host. The custom_target SPIR-V binaries used only by
# integration tests are defined in tests/meson.build (parent) and
# pulled in here as plain variables (subdir() shares scope).

test_smoke = executable('test_smoke',
  'test_smoke.c',
  dependencies : [test_helpers_dep],
)
test('smoke', test_smoke, suite : 'unit')

test_math = executable('test_math',
  'test_math.c',
  dependencies : [test_helpers_dep],
)
test('math', test_math, suite : 'unit')

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

# Headless CPU-canvas GLYPH run test (ADR-0019): draws text on a device-less
# canvas via the host R8 coverage atlas path. Links flux-text for shaping.
test_canvas_cpu_text = executable('test_canvas_cpu_text',
  'test_canvas_cpu_text.c',
  dependencies : [test_helpers_dep, flux_text_dep],
)
test('canvas_cpu_text', test_canvas_cpu_text, suite : 'unit')

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

# Same reasoning as test_canvas_tess: geometry_flatten.c is hidden
# in libflux.so, so we compile it into the test binary directly.
test_canvas_flatten = executable('test_canvas_flatten',
  'test_canvas_flatten.c',
  '../../../libs/flux/src/canvas/geometry_flatten.c',
  dependencies : [test_helpers_dep],
)
test('canvas_flatten', test_canvas_flatten, suite : 'unit')

test_scene_camera = executable('test_scene_camera',
  'test_scene_camera.c',
  dependencies : [test_helpers_dep],
)
test('scene_camera', test_scene_camera, suite : 'unit')

# glyph_cache compiles its single source file directly (matching the
# test_canvas_tess pattern). The cache is a pure data structure with no
# Vulkan / FreeType / Fontconfig dependencies, so this test runs on any
# build host and gates the "candidates lag after a while" regression
# described in CHANGELOG-TEXT.md.
test_glyph_cache = executable('test_glyph_cache',
  'test_glyph_cache.c',
)
test('glyph_cache', test_glyph_cache, suite : 'unit')
