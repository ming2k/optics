# tests/ — orchestrator
#
# Layout:
#   tests/
#   ├── test_helpers.{c,h}  shared test-helper static lib
#   ├── shaders/            SPIR-V sources compiled to .spv here
#   ├── unit/               no Vulkan ICD required
#   ├── integration/        require a VK ICD (lavapipe / real driver)
#   └── bench/              reserved for future meson benchmarks
#
# This file builds the shared helpers + SPIR-V custom_targets once
# and forwards them into the sub-suites via subdir() scope. Tests
# in the suites pick them up directly — no declare_dependency
# indirection needed because the variables stay in scope across the
# subdir() boundary.

# --- shared test helper static library ---
test_helpers_inc = include_directories('.')  # this file lives in tests/
test_helpers_lib = static_library('test_helpers',
  'test_helpers.c',
  dependencies : [flux_dep, m_dep, vulkan_dep],
  install      : false,
)
# --- shared SPIR-V custom_targets (used by integration tests) ---
core_tests_build_dir = meson.current_build_dir()
glslang = find_program('glslangValidator', required : true)

# libdl for the fault-injection test's dlsym(RTLD_NEXT); on glibc >= 2.34
# this resolves to libc, which is fine.
dl_dep = cc.find_library('dl', required : false)

triangle_vert_spv = custom_target('test_triangle.vert.spv',
  input   : 'shaders/triangle.vert',
  output  : 'triangle.vert.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_frag_spv = custom_target('test_triangle.frag.spv',
  input   : 'shaders/triangle.frag',
  output  : 'triangle.frag.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
triangle_attr_vert_spv = custom_target('test_triangle_attr.vert.spv',
  input   : 'shaders/triangle_attr.vert',
  output  : 'triangle_attr.vert.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
fill_pattern_spv = custom_target('test_fill_pattern.comp.spv',
  input   : 'shaders/fill_pattern.comp',
  output  : 'fill_pattern.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
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
spv2h    = files('../../tools/spv2h.py')
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
test_helpers_dep = declare_dependency(
  link_with           : [test_helpers_lib],
  sources             : ['test_helpers.h', test_triangle_vert_h, test_triangle_frag_h,
                         test_triangle_attr_vert_h, test_fill_pattern_h],
  include_directories : test_helpers_inc,
  dependencies        : [flux_dep, m_dep, vulkan_dep],
)

# --- sub-suites ---
subdir('unit')
subdir('integration')
if get_option('fuzz')
  subdir('fuzz')
endif

subdir('bench')
