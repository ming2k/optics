# Caret/selection mapping: every assertion crosses the itemiser + shaper +
# font stack, so this is an integration lane. A host with no shaping backend
# skips explicitly.
test_text_caret = executable('test_text_caret',
  'test_text_caret.c',
  dependencies : [text_test_dep],
)
test('text_caret', test_text_caret, suite : 'integration')

# Real GPU + system font stack text rendering, atlas upload/flush, outline, and
# CJK autospace / BiDi multi-span integration test.
test_text_render_pipeline = executable('test_text_render_pipeline',
  'test_text_render_pipeline.c',
  dependencies : [optics_gfx_test_support_dep, text_dep],
)
test('text_render_pipeline', test_text_render_pipeline, suite : 'integration')

# Long-session text soak: drives the glyph cache past its live ceiling and
# replays resident working sets, asserting the diagnostic counters (evictions,
# hits vs misses, atlas pages/clears) stay in their documented steady states.
# The 9000-glyph working set needs a real CJK face; CI installs Noto CJK on
# Linux. Windows/macOS runner fonts vary, so the lane is Linux-only.
if host_machine.system() == 'linux'
  test_text_soak = executable('test_text_soak',
    'test_text_soak.c',
    dependencies : [text_test_dep],
  )
  test('text_soak', test_text_soak, suite : 'integration', timeout : 120)
endif
