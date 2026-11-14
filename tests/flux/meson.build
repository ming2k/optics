# tests/flux — flux-specific shader products and suite forwarding.
#
# Assertions and Vulkan fixtures live in tests/support. This file owns only
# what is specific to flux tests: SPIR-V custom targets and generated headers.
# The suite directories (unit/, integration/, bench/, fuzz/) consume
# flux_test_support_dep from this scope.

# The assertion harness and device fixtures live in tests/support. This
# subdirectory only owns flux-specific shader build products.
flux_test_inc = include_directories('.')  # shader headers generated in this dir
# --- shared SPIR-V custom_targets (used by integration tests) ---
core_tests_build_dir = meson.current_build_dir()
slangc = find_program('slangc', required : true)

# The single source of truth for the SDF field and its derivative: the same
# header the shipped materials include. Declared as a dependency of every
# target that compiles it, so editing it invalidates the SPIR-V rather than
# leaving a stale byproduct (the ccache trap ADR-0118 records).
flux_sdf_slang = files(meson.project_source_root() / 'libs/flux/include/flux/shaders/flux_sdf.slang')
flux_sdf_inc = '-I' + meson.project_source_root() / 'libs/flux/include/flux/shaders'

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

# --embed-dir is GCC/Clang-only; probe it so MSVC cl can still configure
# (its builds take the generated-header path below instead of #embed).
tests_embed_dir_arg = cc.get_supported_arguments('--embed-dir=' + core_tests_build_dir)

# Generated-header fallback for toolchains without C23 #embed (MSVC cl,
# Apple Clang) — same idiom as libs/flux: the test sources pick per TU
# via __has_embed, so the headers must exist on every host. Header names
# are unique per shader; the array symbol each header defines matches the
# name the test sources already use (vert_spv / frag_spv / fill_spv).
spv2h_py = import('python').find_installation('python3')
spv2h    = files('../../tools/build/spv2h.py')
test_triangle_vert_h = custom_target('triangle_vert_spv.h',
  input   : triangle_vert_spv,
  output  : 'triangle_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'vert_spv'],
)
test_triangle_frag_h = custom_target('triangle_frag_spv.h',
  input   : triangle_frag_spv,
  output  : 'triangle_frag_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'frag_spv'],
)
test_triangle_attr_vert_h = custom_target('triangle_attr_vert_spv.h',
  input   : triangle_attr_vert_spv,
  output  : 'triangle_attr_vert_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'vert_spv'],
)
test_fill_pattern_h = custom_target('fill_pattern_comp_spv.h',
  input   : fill_pattern_spv,
  output  : 'fill_pattern_comp_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'fill_spv'],
)

# Declared after the shader header targets so every consumer of the
# helpers (unit + integration suites) also carries an order-only
# dependency on the generated headers — no per-test source edits needed.
#
# Assertions come from optics_test_dep; Vulkan fixtures from flux_test_dep.
# Keeping the two dependencies separate is what lets a CPU-only test link
# without Vulkan.
flux_test_support_dep = declare_dependency(
  sources             : [test_triangle_vert_h, test_triangle_frag_h,
                         test_triangle_attr_vert_h, test_fill_pattern_h],
  include_directories : flux_test_inc,
  dependencies        : [flux_test_dep, optics_test_dep],
)

# CPU-only unit tests use this dependency. It deliberately does not link the
# Vulkan fixture library or add the shader-header targets: a unit test that
# needs either belongs in integration.
flux_unit_support_dep = declare_dependency(
  dependencies : [flux_dep, m_dep, optics_test_dep],
)

# Coverage acceptance harness (ADR-0119 S0): the independent ground truth every
# coverage backend is measured against. Deliberately NOT part of libflux — a
# backend must not be able to validate itself against its own algorithm.
coverage_harness_lib = static_library('coverage_harness',
  'coverage_harness.c',
  dependencies : [flux_dep, m_dep],
  install      : false,
)
coverage_harness_dep = declare_dependency(
  link_with           : [coverage_harness_lib],
  sources             : ['coverage_harness.h'],
  include_directories : flux_test_inc,
  dependencies        : [flux_unit_support_dep],
)

# --- sub-suites ---
subdir('unit')
subdir('integration')
if get_option('fuzz')
  subdir('fuzz')
endif

subdir('bench')
