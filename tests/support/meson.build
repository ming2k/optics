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

flux_test_lib = static_library(
  'flux_test',
  'flux_test.c',
  dependencies : [flux_dep, m_dep, vulkan_dep],
  install      : false,
)

flux_test_dep = declare_dependency(
  link_with           : flux_test_lib,
  include_directories : include_directories('.'),
  dependencies        : [flux_dep, m_dep, vulkan_dep],
)
