# tests/support — the single test infrastructure layer.
#
# optics_test: assertion accounting only. No Vulkan headers, no library
# dependencies, so every test layer can link it.
#
# flux_test: Vulkan device fixtures shared by flux/prism/vista tests. It is
# separate on purpose: a CPU-only test must not inherit a Vulkan link line just
# to reach an assertion macro.

optics_test_lib = static_library(
  'optics_test',
  'optics_test.c',
  install : false,
)

optics_test_dep = declare_dependency(
  link_with           : optics_test_lib,
  include_directories : include_directories('.'),
)

gfx_test_lib = static_library(
  'gfx_test',
  'gfx_test.c',
  dependencies : [gfx_dep, m_dep, vulkan_dep],
  install      : false,
)

gfx_test_dep = declare_dependency(
  link_with           : gfx_test_lib,
  include_directories : include_directories('.'),
  dependencies        : [gfx_dep, m_dep, vulkan_dep],
)
