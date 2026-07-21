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
test_helpers_dep = declare_dependency(
  link_with           : [test_helpers_lib],
  sources             : ['test_helpers.h'],
  include_directories : test_helpers_inc,
  dependencies        : [flux_dep, m_dep, vulkan_dep],
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

# --- sub-suites ---
subdir('unit')
subdir('integration')
if get_option('fuzz')
  subdir('fuzz')
endif

subdir('bench')
