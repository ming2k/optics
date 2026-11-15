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

subdir('flux')
if get_option('glyph')
  subdir('glyph')
endif
subdir('prism')
subdir('transit')
subdir('vista')
if get_option('reticle')
  subdir('reticle')
endif
if opt_lens
  subdir('lens')
  subdir('iris')
endif
subdir('optics')
subdir('e2e')
