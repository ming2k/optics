# tests — the test topology in one place.
#
# Layers (the test pyramid):
#   unit         one library, deterministic, no GPU, no compositor
#   integration  a real boundary: Vulkan, font stack, multi-library seam
#   e2e          public umbrella API -> observable output, no internals
#
# Orthogonal lanes (not layers): bench, fuzz, lint, tooling.
#
# Every test binary links tests/support/optics_test (assertions) and, when it
# needs a device, tests/support/flux_test (fixtures). No suite owns a private
# copy of CHECK/EXPECT/TEST_REPORT.

subdir('support')

subdir('gfx')
if get_option('text')
  subdir('text')
endif
subdir('material')
subdir('motion')
subdir('scene')
if get_option('svg')
  subdir('svg')
endif
subdir('window')
if opt_ui
  subdir('ui')
  subdir('app')
endif
subdir('optics')
subdir('e2e')
