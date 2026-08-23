# Headless C tests for libiris.
#
# Every test here runs without a Wayland compositor or GPU surface:
#   - test_version       iris_version_string / version macros
#   - test_app_api       iris_app_run / iris_pick_file argument validation
#   - test_theme         colour-scheme query (GTK_THEME branch; gsettings shadowed)
#   - test_uri           file:// URI → path decoding for dialog results
#   - test_a11y_util     AT-SPI role/state maps, value parser, tree diff
#   - test_platform_text UTF-8 boundary-aware truncation helpers
#   - test_platform_input pointer button mapping + scroll-channel policy
#
# The windowed path (iris_app_run with a real build callback) is exercised
# by the examples, not here — it owns an event loop and needs a live session.

src_inc = include_directories('../../libs/iris/src')

tests = ['test_version', 'test_app_api', 'test_theme', 'test_uri', 'test_capability']

foreach t : tests
  exe = executable(t, t + '.c',
    dependencies        : [iris_dep, m_dep],
    include_directories : iris_inc,
  )
  test(t, exe)
endforeach

# test_a11y_util compiles a11y_util.c (+ its platform_text.c dependency)
# directly (the helpers are internal / hidden-visibility, so they are not
# exported from libiris.so).
exe_a11y = executable('test_a11y_util',
  ['test_a11y_util.c', '../../libs/iris/src/a11y_util.c', '../../libs/iris/src/platform_text.c'],
  include_directories : [iris_inc, src_inc],
  dependencies        : [lens_dep, m_dep],
)
test('test_a11y_util', exe_a11y)

# test_a11y_prefs: the public preference API against libiris (query always
# links; watch may be the stub on libsystemd-less builds — both paths are
# contract-valid, the test pins both).
exe_prefs = executable('test_a11y_prefs',
  'test_a11y_prefs.c',
  include_directories : [iris_inc, src_inc],
  dependencies        : [iris_dep, lens_dep, m_dep],
)
test('test_a11y_prefs', exe_prefs)

# test_platform_text also pins the lens_input buffer sizes the backends
# stage into (text_utf8/preedit_utf8), so it needs the lens headers.
exe_ptext = executable('test_platform_text',
  ['test_platform_text.c', '../../libs/iris/src/platform_text.c'],
  include_directories : [iris_inc, src_inc],
  dependencies        : [lens_dep, m_dep],
)
test('test_platform_text', exe_ptext)

# test_platform_input pins the shared pointer-button mapping and the
# scroll-channel classification policy (header-only, so nothing to compile
# in beyond the test itself).
exe_pinput = executable('test_platform_input',
  ['test_platform_input.c'],
  include_directories : [iris_inc, src_inc],
  dependencies        : [lens_dep, m_dep],
)
test('test_platform_input', exe_pinput)
