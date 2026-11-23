# tests/gfx — gfx-specific shader products and suite forwarding.
#
# Assertions and Vulkan fixtures live in tests/support. This file owns only
# what is specific to gfx tests: the SPIR-V custom targets the tests embed.
# The suite directories (unit/, integration/, bench/, fuzz/) consume
# optics_gfx_test_support_dep from this scope.
gfx_test_inc = include_directories('.')
# --- shared SPIR-V custom_targets (used by integration tests) ---
core_tests_build_dir = meson.current_build_dir()
slangc = find_program('slangc', required : true)

# The single source of truth for the SDF field and its derivative: the same
# header the shipped materials include. Declared as a dependency of every
# target that compiles it, so editing it invalidates the SPIR-V rather than
# leaving a stale byproduct (the ccache trap ADR-0118 records).
gfx_sdf_slang = files(meson.project_source_root() / 'libs/gfx/include/optics/gfx/shaders/optics_gfx_sdf.slang')
gfx_sdf_inc = '-I' + meson.project_source_root() / 'libs/gfx/include/optics/gfx/shaders'

# libdl for the fault-injection test's dlsym(RTLD_NEXT); on glibc >= 2.34
# this resolves to libc, which is fine.
dl_dep = cc.find_library('dl', required : false)

triangle_vert_spv = custom_target('test_triangle.vert.spv',
  input   : 'shaders/triangle.vert.slang',
  output  : 'triangle.vert.spv',
  command : [slangc, '-target', 'spirv', '-profile', 'spirv_1_6', '-entry', 'main', '-stage', 'vertex', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_frag_spv = custom_target('test_triangle.frag.spv',
  input   : 'shaders/triangle.frag.slang',
  output  : 'triangle.frag.spv',
  command : [slangc, '-target', 'spirv', '-profile', 'spirv_1_6', '-entry', 'main', '-stage', 'fragment', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_attr_vert_spv = custom_target('test_triangle_attr.vert.spv',
  input   : 'shaders/triangle_attr.vert.slang',
  output  : 'triangle_attr.vert.spv',
  command : [slangc, '-target', 'spirv', '-profile', 'spirv_1_6', '-entry', 'main', '-stage', 'vertex', '@INPUT@', '-o', '@OUTPUT@'],
)
fill_pattern_spv = custom_target('test_fill_pattern.comp.spv',
  input   : 'shaders/fill_pattern.comp.slang',
  output  : 'fill_pattern.comp.spv',
  command : [slangc, '-target', 'spirv', '-profile', 'spirv_1_6', '-entry', 'main', '-stage', 'compute', '@INPUT@', '-o', '@OUTPUT@'],
)

# Test shaders are baked in with C23 #embed, like the library's own. #embed
# searches the including source's directory and then each --embed-dir, so
# every build directory that produces SPIR-V a test embeds is listed: this
# one (shared fixtures), integration/ (per-test shaders) and libs/gfx (the
# shipped tile-raster kernel exercised directly).
tests_embed_dir_arg = [
  '--embed-dir=' + core_tests_build_dir,
  '--embed-dir=' + meson.project_build_root() / 'tests/gfx/integration',
  '--embed-dir=' + meson.project_build_root() / 'libs/gfx',
]

# Every consumer of the helpers (unit + integration suites) carries an
# order-only dependency on the shared SPIR-V, so a test that embeds one never
# compiles before it exists.
#
# Assertions come from optics_test_dep; Vulkan fixtures from gfx_test_dep.
# Keeping the two dependencies separate is what lets a CPU-only test link
# without Vulkan.
optics_gfx_test_support_dep = declare_dependency(
  sources             : [triangle_vert_spv, triangle_frag_spv,
                         triangle_attr_vert_spv, fill_pattern_spv],
  include_directories : gfx_test_inc,
  compile_args        : tests_embed_dir_arg,
  dependencies        : [gfx_test_dep, optics_test_dep],
)

# CPU-only unit tests use this dependency. It deliberately does not link the
# Vulkan fixture library or add the shader-header targets: a unit test that
# needs either belongs in integration.
optics_gfx_unit_support_dep = declare_dependency(
  dependencies : [gfx_dep, m_dep, optics_test_dep],
)

# Coverage acceptance harness (ADR-0119 S0): the independent ground truth every
# coverage backend is measured against. Deliberately NOT part of liboptics_gfx — a
# backend must not be able to validate itself against its own algorithm.
coverage_harness_lib = static_library('coverage_harness',
  'coverage_harness.c',
  dependencies : [gfx_dep, m_dep],
  install      : false,
)
coverage_harness_dep = declare_dependency(
  link_with           : [coverage_harness_lib],
  sources             : ['coverage_harness.h'],
  include_directories : gfx_test_inc,
  dependencies        : [optics_gfx_unit_support_dep],
)

# --- sub-suites ---
subdir('unit')
subdir('integration')
if get_option('fuzz')
  subdir('fuzz')
endif

subdir('bench')
